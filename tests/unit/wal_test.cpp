#include "storage/wal.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>
#include <vector>

#include "support/temp_dir.h"

using namespace orbit::storage;
namespace fs = std::filesystem;

namespace {

WalRecord record(std::uint64_t seq, std::string room = "general") {
    return WalRecord{std::move(room), seq, 1000 + seq, "ada", "message " + std::to_string(seq)};
}

class WalTest : public ::testing::Test {
protected:
    orbit::testing::TempDir dir;

    WalOptions options(SyncPolicy policy = SyncPolicy::None) {
        WalOptions result;
        result.directory = dir.path();
        result.sync_policy = policy;
        return result;
    }

    void write_records(std::uint64_t first, std::uint64_t last) {
        Wal wal(options(), {});
        for (std::uint64_t seq = first; seq <= last; ++seq) {
            ASSERT_TRUE(wal.append(record(seq)));
        }
    }

    std::vector<WalRecord> replay(RecoveryReport* report_out = nullptr) {
        std::vector<WalRecord> records;
        auto report = Wal::recover(dir.path(), [&](const WalRecord& r) { records.push_back(r); });
        if (report_out != nullptr) {
            *report_out = report;
        }
        return records;
    }

    std::vector<fs::path> segments() {
        std::vector<fs::path> paths;
        for (const auto& entry : fs::directory_iterator(dir.path())) {
            paths.push_back(entry.path());
        }
        std::sort(paths.begin(), paths.end());
        return paths;
    }

    static std::vector<char> contents(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    static void overwrite(const fs::path& path, const std::vector<char>& bytes) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
};

}  // namespace

TEST_F(WalTest, EmptyDirectoryRecoversNothing) {
    RecoveryReport report;
    EXPECT_TRUE(replay(&report).empty());
    EXPECT_EQ(report.segments, 0U);
}

TEST_F(WalTest, RecoversRecordsInOrderWithAllFields) {
    write_records(1, 100);
    RecoveryReport report;
    const auto records = replay(&report);

    ASSERT_EQ(records.size(), 100U);
    EXPECT_EQ(report.records, 100U);
    EXPECT_EQ(report.room_heads.at("general"), 100U);
    for (std::uint64_t i = 0; i < records.size(); ++i) {
        EXPECT_EQ(records[i].seq, i + 1);
        EXPECT_EQ(records[i].room, "general");
        EXPECT_EQ(records[i].nick, "ada");
        EXPECT_EQ(records[i].timestamp_ms, 1001 + i);
        EXPECT_EQ(records[i].body, "message " + std::to_string(i + 1));
    }
}

TEST_F(WalTest, KeepsBinarySafeBodies) {
    std::string body("nul\0byte\xFF", 9);
    {
        Wal wal(options(), {});
        ASSERT_TRUE(wal.append(WalRecord{"r", 1, 1, "n", body}));
    }
    const auto records = replay();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].body, body);
}

TEST_F(WalTest, RestartAppendsAfterExistingData) {
    write_records(1, 5);
    write_records(6, 10);
    const auto records = replay();
    ASSERT_EQ(records.size(), 10U);
    EXPECT_EQ(records.back().seq, 10U);
    EXPECT_EQ(segments().size(), 2U);
}

// Simulates a crash at every possible point in the final record: however much of
// it reached the disk, recovery must keep everything before it and nothing after.
TEST_F(WalTest, TruncatesTornTailAtEveryByteOffset) {
    write_records(1, 3);
    const auto path = segments().at(0);
    const auto intact = contents(path);

    // Measure where the last record starts by writing two records instead.
    orbit::testing::TempDir other;
    {
        WalOptions shorter = options();
        shorter.directory = other.path();
        Wal wal(shorter, {});
        ASSERT_TRUE(wal.append(record(1)));
        ASSERT_TRUE(wal.append(record(2)));
    }
    std::size_t two_records_size = 0;
    for (const auto& entry : fs::directory_iterator(other.path())) {
        two_records_size = static_cast<std::size_t>(entry.file_size());
    }

    for (std::size_t size = two_records_size; size < intact.size(); ++size) {
        overwrite(path, std::vector<char>(intact.begin(),
                                          intact.begin() + static_cast<std::ptrdiff_t>(size)));
        RecoveryReport report;
        const auto records = replay(&report);
        ASSERT_EQ(records.size(), 2U) << "at size " << size;
        EXPECT_EQ(report.truncated_bytes, size - two_records_size);
        EXPECT_EQ(fs::file_size(path), two_records_size);
    }
}

TEST_F(WalTest, DetectsBitRotInTheNewestRecord) {
    write_records(1, 3);
    const auto path = segments().at(0);
    auto bytes = contents(path);
    bytes[bytes.size() - 2] ^= 0x40;
    overwrite(path, bytes);

    const auto records = replay();
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(records.back().seq, 2U);
}

TEST_F(WalTest, RecoveryIsIdempotent) {
    write_records(1, 3);
    const auto path = segments().at(0);
    auto bytes = contents(path);
    bytes.resize(bytes.size() - 3);
    overwrite(path, bytes);

    EXPECT_EQ(replay().size(), 2U);
    RecoveryReport second;
    EXPECT_EQ(replay(&second).size(), 2U);
    EXPECT_EQ(second.truncated_bytes, 0U);
}

TEST_F(WalTest, DamageInAnOlderSegmentDoesNotLoseNewerSegments) {
    write_records(1, 10);
    write_records(11, 20);
    const auto first = segments().at(0);
    auto bytes = contents(first);
    bytes[bytes.size() / 2] ^= 0x01;
    overwrite(first, bytes);

    RecoveryReport report;
    const auto records = replay(&report);
    EXPECT_EQ(report.corrupt_segments, 1U);
    EXPECT_EQ(report.truncated_bytes, 0U);
    EXPECT_EQ(contents(first), bytes);  // Left in place for forensics.
    ASSERT_GE(records.size(), 10U);
    EXPECT_EQ(records.back().seq, 20U);
}

TEST_F(WalTest, RejectsSegmentWithForeignHeader) {
    write_records(1, 3);
    const auto path = segments().at(0);
    auto bytes = contents(path);
    bytes[0] = 'X';
    overwrite(path, bytes);
    EXPECT_TRUE(replay().empty());
}

TEST_F(WalTest, RotatesSegmentsAtTheSizeLimit) {
    auto opts = options();
    opts.segment_bytes = 1024;
    {
        Wal wal(opts, {});
        for (std::uint64_t seq = 1; seq <= 2000; ++seq) {
            ASSERT_TRUE(wal.append(record(seq)));
            if (seq % 50 == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
    EXPECT_GT(segments().size(), 3U);
    const auto records = replay();
    ASSERT_EQ(records.size(), 2000U);
    for (std::uint64_t i = 0; i < records.size(); ++i) {
        EXPECT_EQ(records[i].seq, i + 1);
    }
}

// Pruning deletes history but must not rewind sequence numbers: reusing one would
// make a reconnecting client silently skip new messages.
TEST_F(WalTest, RoomHeadSurvivesDeletionOfItsMessages) {
    auto opts = options();
    opts.segment_bytes = 512;
    opts.max_segments = 2;
    {
        Wal wal(opts, {});
        ASSERT_TRUE(wal.append(record(77, "quiet")));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        for (std::uint64_t seq = 1; seq <= 500; ++seq) {
            ASSERT_TRUE(wal.append(record(seq, "busy")));
            if (seq % 10 == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
    EXPECT_LE(segments().size(), 2U);

    RecoveryReport report;
    const auto records = replay(&report);
    EXPECT_LT(records.size(), 501U);
    EXPECT_EQ(report.room_heads.at("quiet"), 77U);
    EXPECT_EQ(report.room_heads.at("busy"), 500U);
}

TEST_F(WalTest, HeadsFromRecoveryAreCarriedForward) {
    {
        Wal wal(options(), {{"carried", 900}});
        ASSERT_TRUE(wal.append(record(1, "other")));
    }
    RecoveryReport report;
    replay(&report);
    EXPECT_EQ(report.room_heads.at("carried"), 900U);
}

TEST_F(WalTest, AlwaysPolicyCompletesOnlyAfterSync) {
    orbit::metrics::Registry registry;
    auto opts = options(SyncPolicy::Always);
    Wal wal(opts, {}, &registry);

    std::promise<bool> done;
    ASSERT_TRUE(wal.append(record(1), [&](bool durable) { done.set_value(durable); }));
    EXPECT_TRUE(done.get_future().get());
    EXPECT_NE(registry.render().find("orbit_wal_fsyncs_total"), std::string::npos);
}

TEST_F(WalTest, GroupCommitCoversConcurrentAppends) {
    constexpr int kThreads = 8;
    constexpr int kPerThread = 200;

    orbit::metrics::Registry registry;
    auto opts = options(SyncPolicy::Always);
    std::atomic<int> durable{0};
    {
        Wal wal(opts, {}, &registry);
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < kPerThread; ++i) {
                    const auto seq = static_cast<std::uint64_t>(i + 1);
                    EXPECT_TRUE(wal.append(record(seq, "room" + std::to_string(t)),
                                           [&](bool ok) { durable += ok ? 1 : 0; }));
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
    }
    EXPECT_EQ(durable.load(), kThreads * kPerThread);
    EXPECT_EQ(replay().size(), static_cast<std::size_t>(kThreads * kPerThread));
}

TEST_F(WalTest, RefusesAppendsBeyondTheBacklogLimit) {
    auto opts = options();
    opts.max_pending_bytes = 1;
    Wal wal(opts, {});
    EXPECT_FALSE(wal.append(record(1)));
}

TEST_F(WalTest, CloseIsIdempotent) {
    Wal wal(options(), {});
    wal.close();
    wal.close();
}
