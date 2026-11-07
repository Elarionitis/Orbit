#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/metrics.h"
#include "net/socket.h"

namespace orbit::storage {

enum class SyncPolicy {
    // None and Interval acknowledge a record once it is queued, before the writer
    // thread has written it, so even a process crash can lose the queued tail.
    None,      // Leave flushing to the OS.
    Interval,  // fsync on a timer. Bounds loss on power failure to the interval.
    Always,    // fsync before acknowledging. Nothing acknowledged can be lost.
};

struct WalOptions {
    std::filesystem::path directory;
    SyncPolicy sync_policy = SyncPolicy::Interval;
    std::uint64_t sync_interval_ms = 100;
    std::uint64_t segment_bytes = 64ULL * 1024 * 1024;
    std::size_t max_segments = 0;  // Oldest segments are deleted past this. Zero keeps all.
    std::size_t max_pending_bytes = 64ULL * 1024 * 1024;
};

struct WalRecord {
    std::string room;
    std::uint64_t seq = 0;
    std::uint64_t timestamp_ms = 0;
    std::string nick;
    std::string body;
};

struct RecoveryReport {
    std::size_t segments = 0;
    std::size_t records = 0;
    std::size_t truncated_bytes = 0;   // Torn tail removed from the newest segment.
    std::size_t corrupt_segments = 0;  // Older segments with damage in the middle.
    std::map<std::string, std::uint64_t> room_heads;
};

// Append-only, segmented write-ahead log.
//
// A segment is a 16-byte header followed by records:
//     u32 payload length | u32 CRC-32C of payload | payload
//
// One dedicated thread owns the file. Producers hand it records and return
// immediately; it drains whatever has accumulated into a single write() and, in
// Always mode, a single fsync(). That is group commit: under load one fsync
// covers a whole batch, so throughput rises with concurrency while the
// durability guarantee stays exactly the same.
class Wal {
public:
    using Completion = std::function<void(bool durable)>;
    using Visitor = std::function<void(const WalRecord&)>;

    // Replays every intact record in order. A torn record at the end of the
    // newest segment is what a crash leaves behind, so it is truncated away.
    // Damage anywhere else is reported and skipped rather than treated as fatal.
    static RecoveryReport recover(const std::filesystem::path& directory, const Visitor& visitor);

    // Opens a fresh segment after the newest one on disk, seeded with `heads` so
    // that sequence numbers survive the deletion of older segments.
    Wal(WalOptions options, std::map<std::string, std::uint64_t> heads,
        metrics::Registry* registry = nullptr);
    ~Wal();

    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;

    // Thread-safe. Returns false without queuing if the backlog limit has been
    // reached or the log has failed. `on_done` runs on the writer thread once the
    // record is written and, in Always mode, synced.
    bool append(WalRecord record, Completion on_done = {});

    // Drains the queue, syncs, and stops the writer. Called by the destructor.
    void close();

    bool failed() const { return m_failed.load(std::memory_order_acquire); }

private:
    struct Pending {
        WalRecord record;
        Completion on_done;
    };

    void writer_loop();
    bool write_batch(std::vector<Pending>& batch);
    bool write_buffer();
    bool sync_file();
    bool open_segment();
    void prune_segments();

    WalOptions m_options;
    std::map<std::string, std::uint64_t> m_heads;  // Writer thread only after construction.

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::vector<Pending> m_queue;  // Guarded by m_mutex.
    bool m_stopping = false;       // Guarded by m_mutex.
    std::atomic<std::size_t> m_pending_bytes{0};
    std::atomic<bool> m_failed{false};

    net::UniqueFd m_file;
    std::uint64_t m_segment_id = 0;
    std::uint64_t m_segment_size = 0;
    std::uint64_t m_last_sync_ms = 0;
    bool m_dirty = false;
    std::vector<std::uint8_t> m_buffer;

    metrics::Counter* m_records_metric = nullptr;
    metrics::Counter* m_bytes_metric = nullptr;
    metrics::Counter* m_syncs_metric = nullptr;
    metrics::Histogram* m_batch_metric = nullptr;
    metrics::Histogram* m_sync_latency_metric = nullptr;

    std::thread m_thread;
};

const char* to_string(SyncPolicy policy);

}  // namespace orbit::storage
