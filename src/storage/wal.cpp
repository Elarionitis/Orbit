#include "storage/wal.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "common/clock.h"
#include "common/crc32c.h"
#include "common/log.h"
#include "protocol/wire.h"

namespace orbit::storage {
namespace {

namespace fs = std::filesystem;
using protocol::Reader;
using protocol::Writer;

constexpr std::array<std::uint8_t, 8> kMagic = {'O', 'R', 'B', 'I', 'T', 'W', 'A', 'L'};
constexpr std::uint32_t kFormatVersion = 1;
constexpr std::size_t kHeaderSize = 16;
constexpr std::size_t kRecordPrefix = 8;
constexpr std::size_t kMaxRecordSize = 16 * 1024 * 1024;

constexpr std::uint8_t kTypeMessage = 1;
constexpr std::uint8_t kTypeRoomHead = 2;

fs::path segment_path(const fs::path& directory, std::uint64_t id) {
    return directory / std::format("wal-{:016}.log", id);
}

std::vector<std::pair<std::uint64_t, fs::path>> list_segments(const fs::path& directory) {
    std::vector<std::pair<std::uint64_t, fs::path>> segments;
    if (!fs::exists(directory)) {
        return segments;
    }
    for (const auto& entry : fs::directory_iterator(directory)) {
        const std::string name = entry.path().filename().string();
        if (name.size() == 24 && name.starts_with("wal-") && name.ends_with(".log")) {
            try {
                segments.emplace_back(std::stoull(name.substr(4, 16)), entry.path());
            } catch (const std::exception&) {
                log::warn("ignoring unrecognised file in WAL directory: {}", name);
            }
        }
    }
    std::sort(segments.begin(), segments.end());
    return segments;
}

void put_u32(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value >> 24);
    out[1] = static_cast<std::uint8_t>(value >> 16);
    out[2] = static_cast<std::uint8_t>(value >> 8);
    out[3] = static_cast<std::uint8_t>(value);
}

std::uint32_t get_u32(const std::uint8_t* in) {
    return static_cast<std::uint32_t>(in[0]) << 24 | static_cast<std::uint32_t>(in[1]) << 16 |
           static_cast<std::uint32_t>(in[2]) << 8 | static_cast<std::uint32_t>(in[3]);
}

// Reserves the record prefix, lets `fill` write the payload, then backfills the
// length and checksum. The record is framed in place without a second buffer.
template <typename Fill>
void frame_record(std::vector<std::uint8_t>& out, Fill&& fill) {
    const std::size_t start = out.size();
    out.resize(start + kRecordPrefix);
    Writer writer(out);
    fill(writer);
    const std::size_t payload_size = out.size() - start - kRecordPrefix;
    const std::span<const std::uint8_t> payload(out.data() + start + kRecordPrefix, payload_size);
    put_u32(out.data() + start, static_cast<std::uint32_t>(payload_size));
    put_u32(out.data() + start + 4, crc32c(payload));
}

void encode_message(std::vector<std::uint8_t>& out, const WalRecord& record) {
    frame_record(out, [&](Writer& writer) {
        writer.u8(kTypeMessage);
        writer.str(record.room);
        writer.u64(record.seq);
        writer.u64(record.timestamp_ms);
        writer.str(record.nick);
        writer.blob(record.body);
    });
}

void encode_room_head(std::vector<std::uint8_t>& out, const std::string& room, std::uint64_t head) {
    frame_record(out, [&](Writer& writer) {
        writer.u8(kTypeRoomHead);
        writer.str(room);
        writer.u64(head);
    });
}

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error(std::format("cannot read {}", path.string()));
    }
    const auto size = static_cast<std::size_t>(file.tellg());
    std::vector<std::uint8_t> bytes(size);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    return bytes;
}

// Scans a segment and returns the offset just past the last valid record.
std::size_t scan_segment(std::span<const std::uint8_t> bytes, RecoveryReport& report,
                         const Wal::Visitor& visitor) {
    if (bytes.size() < kHeaderSize || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin()) ||
        get_u32(bytes.data() + kMagic.size()) != kFormatVersion) {
        return 0;
    }

    std::size_t offset = kHeaderSize;
    while (bytes.size() - offset >= kRecordPrefix) {
        const std::uint32_t length = get_u32(bytes.data() + offset);
        const std::uint32_t expected_crc = get_u32(bytes.data() + offset + 4);
        if (length == 0 || length > kMaxRecordSize ||
            bytes.size() - offset - kRecordPrefix < length) {
            break;
        }
        const auto payload = bytes.subspan(offset + kRecordPrefix, length);
        if (crc32c(payload) != expected_crc) {
            break;
        }

        Reader reader(payload);
        const std::uint8_t type = reader.u8();
        if (type == kTypeMessage) {
            WalRecord record;
            record.room = reader.str();
            record.seq = reader.u64();
            record.timestamp_ms = reader.u64();
            record.nick = reader.str();
            record.body = reader.blob();
            if (!reader.ok()) {
                break;
            }
            auto& head = report.room_heads[record.room];
            head = std::max(head, record.seq);
            ++report.records;
            visitor(record);
        } else if (type == kTypeRoomHead) {
            const std::string room = reader.str();
            const std::uint64_t seq = reader.u64();
            if (!reader.ok()) {
                break;
            }
            auto& head = report.room_heads[room];
            head = std::max(head, seq);
        } else {
            break;
        }
        offset += kRecordPrefix + length;
    }
    return offset;
}

}  // namespace

const char* to_string(SyncPolicy policy) {
    switch (policy) {
        case SyncPolicy::None: return "none";
        case SyncPolicy::Interval: return "interval";
        case SyncPolicy::Always: return "always";
    }
    return "unknown";
}

RecoveryReport Wal::recover(const fs::path& directory, const Visitor& visitor) {
    RecoveryReport report;
    const auto segments = list_segments(directory);
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const auto& path = segments[i].second;
        const auto bytes = read_file(path);
        const std::size_t valid = scan_segment(bytes, report, visitor);
        ++report.segments;
        if (valid == bytes.size()) {
            continue;
        }
        if (i + 1 == segments.size()) {
            // The newest segment is the only one a crash can have left mid-write.
            report.truncated_bytes = bytes.size() - valid;
            fs::resize_file(path, valid);
            log::warn("WAL: truncated {} torn bytes from {}", report.truncated_bytes,
                      path.filename().string());
        } else {
            ++report.corrupt_segments;
            log::error("WAL: {} is damaged at offset {}; skipping the remaining {} bytes",
                       path.filename().string(), valid, bytes.size() - valid);
        }
    }
    return report;
}

Wal::Wal(WalOptions options, std::map<std::string, std::uint64_t> heads,
         metrics::Registry* registry)
    : m_options(std::move(options)), m_heads(std::move(heads)) {
    fs::create_directories(m_options.directory);
    const auto segments = list_segments(m_options.directory);
    m_segment_id = segments.empty() ? 0 : segments.back().first;

    if (registry != nullptr) {
        m_records_metric = &registry->counter("orbit_wal_records_total", "Records written.");
        m_bytes_metric = &registry->counter("orbit_wal_bytes_total", "Bytes written.");
        m_syncs_metric = &registry->counter("orbit_wal_fsyncs_total", "fsync calls issued.");
        m_batch_metric = &registry->histogram("orbit_wal_batch_records",
                                              "Records per group commit.",
                                              {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024});
        m_sync_latency_metric = &registry->histogram(
            "orbit_wal_fsync_seconds", "fsync latency.",
            {0.0001, 0.0005, 0.001, 0.0025, 0.005, 0.01, 0.025, 0.05, 0.1, 0.5});
    }

    if (!open_segment()) {
        throw std::system_error(errno, std::generic_category(), "cannot open WAL segment");
    }
    m_last_sync_ms = monotonic_ms();
    m_thread = std::thread([this] { writer_loop(); });
}

Wal::~Wal() {
    close();
}

bool Wal::append(WalRecord record, Completion on_done) {
    const std::size_t size = record.body.size() + record.room.size() + record.nick.size() + 32;
    if (failed() ||
        m_pending_bytes.load(std::memory_order_relaxed) + size > m_options.max_pending_bytes) {
        return false;
    }
    m_pending_bytes.fetch_add(size, std::memory_order_relaxed);
    {
        const std::lock_guard lock(m_mutex);
        m_queue.push_back(Pending{std::move(record), std::move(on_done)});
    }
    m_wake.notify_one();
    return true;
}

void Wal::close() {
    {
        const std::lock_guard lock(m_mutex);
        m_stopping = true;
    }
    m_wake.notify_one();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void Wal::writer_loop() {
    std::vector<Pending> batch;
    for (;;) {
        bool stopping = false;
        {
            std::unique_lock lock(m_mutex);
            const auto ready = [this] { return !m_queue.empty() || m_stopping; };
            if (m_options.sync_policy == SyncPolicy::Interval && m_dirty) {
                m_wake.wait_for(lock, std::chrono::milliseconds(m_options.sync_interval_ms), ready);
            } else {
                m_wake.wait(lock, ready);
            }
            batch.swap(m_queue);
            stopping = m_stopping;
        }

        const bool written = write_batch(batch);

        bool synced = written;
        if (written && m_dirty) {
            const bool sync_due = m_options.sync_policy == SyncPolicy::Always ||
                                  (m_options.sync_policy == SyncPolicy::Interval &&
                                   monotonic_ms() - m_last_sync_ms >= m_options.sync_interval_ms);
            if (sync_due || stopping) {
                synced = sync_file();
            }
        }
        if (!synced) {
            m_failed.store(true, std::memory_order_release);
        }

        for (Pending& pending : batch) {
            if (pending.on_done) {
                pending.on_done(synced);
            }
        }
        batch.clear();

        if (synced && m_segment_size >= m_options.segment_bytes) {
            if (m_dirty) {
                synced = sync_file();
            }
            if (!synced || !open_segment()) {
                m_failed.store(true, std::memory_order_release);
            }
        }
        if (stopping) {
            return;
        }
    }
}

bool Wal::write_batch(std::vector<Pending>& batch) {
    if (batch.empty()) {
        return true;
    }
    m_buffer.clear();
    std::size_t queued_size = 0;
    for (const Pending& pending : batch) {
        const WalRecord& record = pending.record;
        encode_message(m_buffer, record);
        auto& head = m_heads[record.room];
        head = std::max(head, record.seq);
        queued_size += record.body.size() + record.room.size() + record.nick.size() + 32;
    }
    m_pending_bytes.fetch_sub(queued_size, std::memory_order_relaxed);

    if (failed()) {
        return false;
    }
    if (m_batch_metric != nullptr) {
        m_batch_metric->observe(static_cast<double>(batch.size()));
        m_records_metric->add(batch.size());
        m_bytes_metric->add(m_buffer.size());
    }
    return write_buffer();
}

bool Wal::write_buffer() {
    std::size_t offset = 0;
    while (offset < m_buffer.size()) {
        const ssize_t count =
            ::write(m_file.get(), m_buffer.data() + offset, m_buffer.size() - offset);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            log::error("WAL write failed: {}", std::strerror(errno));
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    m_segment_size += m_buffer.size();
    m_dirty = true;
    return true;
}

bool Wal::sync_file() {
    const std::uint64_t start = monotonic_ns();
#if defined(__APPLE__)
    // On macOS plain fsync() stops at the drive's volatile cache.
    const int result = ::fcntl(m_file.get(), F_FULLFSYNC);
#elif defined(__linux__)
    const int result = ::fdatasync(m_file.get());
#else
    const int result = ::fsync(m_file.get());
#endif
    if (result != 0) {
        // After a failed fsync the kernel may have dropped the dirty pages, so
        // retrying could falsely report success. Fail the log instead.
        log::error("WAL fsync failed: {}", std::strerror(errno));
        return false;
    }
    m_dirty = false;
    m_last_sync_ms = monotonic_ms();
    if (m_syncs_metric != nullptr) {
        m_syncs_metric->add();
        m_sync_latency_metric->observe(static_cast<double>(monotonic_ns() - start) / 1e9);
    }
    return true;
}

bool Wal::open_segment() {
    ++m_segment_id;
    const fs::path path = segment_path(m_options.directory, m_segment_id);
    net::UniqueFd file(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644));
    if (!file.valid()) {
        log::error("cannot create {}: {}", path.string(), std::strerror(errno));
        return false;
    }
    m_file = std::move(file);
    m_segment_size = 0;

    m_buffer.assign(kHeaderSize, 0);
    std::copy(kMagic.begin(), kMagic.end(), m_buffer.begin());
    put_u32(m_buffer.data() + kMagic.size(), kFormatVersion);

    // Each segment restates every room's head, so an older segment can be
    // deleted without sequence numbers rewinding.
    for (const auto& [room, head] : m_heads) {
        encode_room_head(m_buffer, room, head);
    }
    if (!write_buffer() || !sync_file()) {
        return false;
    }

    // A new file is only durable once its directory entry is.
    const net::UniqueFd directory(::open(m_options.directory.c_str(), O_RDONLY | O_CLOEXEC));
    if (directory.valid()) {
        ::fsync(directory.get());
    }
    prune_segments();
    return true;
}

void Wal::prune_segments() {
    if (m_options.max_segments == 0) {
        return;
    }
    const auto segments = list_segments(m_options.directory);
    if (segments.size() <= m_options.max_segments) {
        return;
    }
    const std::size_t excess = segments.size() - m_options.max_segments;
    for (std::size_t i = 0; i < excess; ++i) {
        std::error_code error;
        fs::remove(segments[i].second, error);
        if (error) {
            log::warn("cannot remove {}: {}", segments[i].second.string(), error.message());
        }
    }
}

}  // namespace orbit::storage
