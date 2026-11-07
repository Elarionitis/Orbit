// Kills a real server process with SIGKILL while it is under load, then checks
// the promise fsync=always makes: every acknowledged message survives.
#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "storage/wal.h"
#include "support/temp_dir.h"
#include "support/test_client.h"

using namespace orbit::protocol;
using orbit::testing::TempDir;
using orbit::testing::TestClient;

namespace {

std::uint16_t free_port() {
    const auto listener = orbit::net::listen_tcp("127.0.0.1", 0, 1);
    return orbit::net::local_port(listener.get());
}

class ServerProcess {
public:
    ServerProcess(const std::filesystem::path& data_dir, std::uint16_t port) {
        m_pid = ::fork();
        if (m_pid == 0) {
            const std::string port_text = std::to_string(port);
            ::execl(ORBIT_SERVER_BINARY, "orbit_server", "--port", port_text.c_str(), "--threads",
                    "4", "--data-dir", data_dir.c_str(), "--fsync", "always", "--rate", "0",
                    "--log-level", "error", static_cast<char*>(nullptr));
            ::_exit(127);
        }
    }

    ~ServerProcess() {
        if (m_pid > 0) {
            ::kill(m_pid, SIGKILL);
            ::waitpid(m_pid, nullptr, 0);
        }
    }

    void kill_hard() {
        ::kill(m_pid, SIGKILL);
        ::waitpid(m_pid, nullptr, 0);
        m_pid = -1;
    }

    int terminate_gracefully() {
        ::kill(m_pid, SIGTERM);
        int status = 0;
        ::waitpid(m_pid, &status, 0);
        m_pid = -1;
        return status;
    }

private:
    pid_t m_pid = -1;
};

std::optional<TestClient> try_login(std::uint16_t port, const std::string& nick) {
    for (int attempt = 0; attempt < 500; ++attempt) {
        try {
            TestClient client(port);
            client.send(Hello{kProtocolVersion, nick, ""});
            auto reply = client.receive(1000);
            if (reply.has_value() && std::holds_alternative<Welcome>(*reply)) {
                return client;
            }
        } catch (const std::exception&) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return std::nullopt;
}

}  // namespace

TEST(CrashRecovery, EveryAcknowledgedMessageSurvivesSigkill) {
    TempDir dir;
    const std::uint16_t port = free_port();
    std::mt19937 random(20261107);

    std::map<std::uint64_t, std::string> acknowledged;  // seq -> body
    std::uint64_t next_id = 0;

    for (int round = 0; round < 5; ++round) {
        ServerProcess server(dir.path(), port);
        auto client = try_login(port, "writer");
        ASSERT_TRUE(client.has_value()) << "server did not come up in round " << round;
        client->send(Join{"ledger", 0});
        ASSERT_TRUE(client->wait_for<Joined>().has_value());

        // Publish from this thread, killing the server from another mid-stream.
        std::uniform_int_distribution<int> delay_ms(20, 120);
        std::thread killer([&, delay = delay_ms(random)] {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            server.kill_hard();
        });

        std::map<std::uint64_t, std::string> in_flight;  // correlation -> body
        for (;;) {
            for (int i = 0; i < 16; ++i) {
                const std::uint64_t id = ++next_id;
                const std::string body = "payload-" + std::to_string(id);
                in_flight[id] = body;
                client->send(Publish{id, "ledger", body});
            }
            // Read acks until this batch is settled or the connection dies.
            bool alive = true;
            while (!in_flight.empty() && alive) {
                auto reply = client->receive(2000);
                if (!reply.has_value()) {
                    alive = false;
                } else if (const auto* ack = std::get_if<Ack>(&*reply)) {
                    const auto found = in_flight.find(ack->correlation);
                    ASSERT_NE(found, in_flight.end());
                    acknowledged[ack->seq] = found->second;
                    in_flight.erase(found);
                }
            }
            if (!alive) {
                break;
            }
        }
        killer.join();
    }

    ASSERT_GT(acknowledged.size(), 0U) << "no messages were acknowledged, so nothing was tested";

    // Replay from a fresh server. History must cover everything acknowledged.
    ServerProcess server(dir.path(), port);
    auto reader = try_login(port, "auditor");
    ASSERT_TRUE(reader.has_value());
    reader->send(Join{"ledger", 0});
    const auto joined = reader->wait_for<Joined>();
    ASSERT_TRUE(joined.has_value());
    EXPECT_GE(joined->head_seq, acknowledged.rbegin()->first);

    std::map<std::uint64_t, std::string> recovered;
    std::uint64_t previous = 0;
    while (previous < joined->head_seq) {
        const auto message = reader->wait_for<Message>(5000);
        ASSERT_TRUE(message.has_value()) << "history ended after seq " << previous;
        ASSERT_GT(message->seq, previous) << "history is out of order";
        previous = message->seq;
        recovered[message->seq] = message->body;
    }

    // Live history is capped, so audit the complete record from the log itself:
    // every message the server ever acknowledged must be there, unaltered.
    server.terminate_gracefully();
    std::map<std::uint64_t, std::string> on_disk;
    const auto report =
        orbit::storage::Wal::recover(dir.path(), [&](const orbit::storage::WalRecord& record) {
            EXPECT_EQ(record.room, "ledger");
            on_disk[record.seq] = record.body;
        });
    EXPECT_EQ(report.corrupt_segments, 0U);

    for (const auto& [seq, body] : acknowledged) {
        const auto found = on_disk.find(seq);
        ASSERT_NE(found, on_disk.end()) << "acknowledged seq " << seq << " was lost";
        ASSERT_EQ(found->second, body) << "acknowledged seq " << seq << " was altered";
    }
    std::cout << "verified all " << acknowledged.size()
              << " acknowledged messages after 5 SIGKILLs\n";
}

TEST(GracefulShutdown, SigtermExitsCleanly) {
    TempDir dir;
    const std::uint16_t port = free_port();
    ServerProcess server(dir.path(), port);
    auto client = try_login(port, "ada");
    ASSERT_TRUE(client.has_value());

    const int status = server.terminate_gracefully();
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}
