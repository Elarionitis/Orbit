#include "client/chat_client.h"

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include "support/server_fixture.h"
#ifdef ORBIT_HAS_TLS
#include "net/tls.h"
#include "support/test_certificate.h"
#endif

using namespace orbit;
using namespace orbit::protocol;
using client::ChatClient;
using orbit::testing::ServerTest;
using orbit::testing::TempDir;
using orbit::testing::TestClient;

namespace {

// Runs a ChatClient on its own event loop and records what it observes.
class Harness {
public:
    explicit Harness(ChatClient::Options options) : m_options(std::move(options)) {
        m_options.min_backoff_ms = 20;
        m_options.max_backoff_ms = 100;
        m_thread = std::thread([this] { m_loop.run(); });
        call([this] {
            ChatClient::Events events;
            events.on_state = [this](ChatClient::State state, const std::string& detail) {
                const std::lock_guard lock(m_mutex);
                m_state = state;
                m_detail = detail;
                if (state == ChatClient::State::Online) {
                    ++m_times_online;
                }
            };
            events.on_message = [this](const Message& message, bool history) {
                const std::lock_guard lock(m_mutex);
                m_messages.push_back(message);
                m_history_flags.push_back(history);
            };
            events.on_joined = [this](const Joined&, std::uint64_t missed) {
                const std::lock_guard lock(m_mutex);
                ++m_joins;
                m_missed += missed;
            };
            m_client = std::make_unique<ChatClient>(m_loop, m_options, std::move(events));
        });
    }

    ~Harness() {
        call([this] { m_client->stop(); });
        call([this] { m_client.reset(); });
        m_loop.stop();
        m_thread.join();
    }

    template <typename F>
    void call(F&& work) {
        std::promise<void> done;
        m_loop.post([&] {
            work();
            done.set_value();
        });
        done.get_future().wait();
    }

    ChatClient& client() { return *m_client; }

    ChatClient::State state() {
        const std::lock_guard lock(m_mutex);
        return m_state;
    }
    std::string detail() {
        const std::lock_guard lock(m_mutex);
        return m_detail;
    }
    int times_online() {
        const std::lock_guard lock(m_mutex);
        return m_times_online;
    }
    int joins() {
        const std::lock_guard lock(m_mutex);
        return m_joins;
    }
    std::uint64_t missed() {
        const std::lock_guard lock(m_mutex);
        return m_missed;
    }
    std::vector<Message> messages() {
        const std::lock_guard lock(m_mutex);
        return m_messages;
    }
    std::vector<bool> history_flags() {
        const std::lock_guard lock(m_mutex);
        return m_history_flags;
    }

private:
    ChatClient::Options m_options;
    net::EventLoop m_loop;
    std::thread m_thread;
    std::unique_ptr<ChatClient> m_client;

    std::mutex m_mutex;
    ChatClient::State m_state = ChatClient::State::Idle;
    std::string m_detail;
    int m_times_online = 0;
    int m_joins = 0;
    std::uint64_t m_missed = 0;
    std::vector<Message> m_messages;
    std::vector<bool> m_history_flags;
};

ChatClient::Options options_for(std::uint16_t port, const std::string& nick) {
    ChatClient::Options options;
    options.port = port;
    options.nick = nick;
    return options;
}

}  // namespace

TEST_F(ServerTest, ClientConnectsJoinsAndPublishes) {
    start_server();
    Harness harness(options_for(port(), "ada"));
    harness.call([&] {
        harness.client().join("general");
        harness.client().start();
    });
    ASSERT_TRUE(eventually([&] { return harness.joins() == 1; }));

    harness.call([&] { EXPECT_TRUE(harness.client().publish("general", "hello")); });
    ASSERT_TRUE(eventually([&] { return harness.messages().size() == 1; }));
    EXPECT_EQ(harness.messages()[0].body, "hello");
    EXPECT_FALSE(harness.history_flags()[0]);
}

TEST_F(ServerTest, ClientFlagsReplayedHistory) {
    start_server();
    auto seeder = login("seeder");
    seeder.join("general");
    seeder.send(Publish{1, "general", "old news"});
    ASSERT_TRUE(seeder.wait_for<Ack>().has_value());

    Harness harness(options_for(port(), "ada"));
    harness.call([&] {
        harness.client().join("general");
        harness.client().start();
    });
    ASSERT_TRUE(eventually([&] { return harness.messages().size() == 1; }));
    EXPECT_TRUE(harness.history_flags()[0]);
}

TEST_F(ServerTest, FatalLoginErrorsStopRetrying) {
    start_server();
    auto holder = login("ada");

    Harness harness(options_for(port(), "ada"));
    harness.call([&] { harness.client().start(); });
    ASSERT_TRUE(eventually([&] { return harness.state() == ChatClient::State::Stopped; }));
    EXPECT_NE(harness.detail().find("nickname"), std::string::npos);
}

TEST_F(ServerTest, ClientRetriesUntilTheServerAppears) {
    std::uint16_t reserved = 0;
    {
        auto listener = net::listen_tcp("127.0.0.1", 0, 1);
        reserved = net::local_port(listener.get());
    }
    Harness harness(options_for(reserved, "ada"));
    harness.call([&] { harness.client().start(); });
    ASSERT_TRUE(eventually([&] { return harness.state() == ChatClient::State::Backoff; }));

    config.port = reserved;
    start_server();
    EXPECT_TRUE(eventually([&] { return harness.state() == ChatClient::State::Online; }));
}

// The end-to-end recovery story. A client is connected, the server goes away, more
// messages are written while the client is offline, and on reconnecting it receives
// exactly what it missed: nothing lost, nothing shown twice.
class ReconnectTest : public ServerTest {
protected:
    ReconnectTest() {
        config.data_dir = dir.path();
        config.sync_policy = storage::SyncPolicy::None;
    }
    TempDir dir;
};

TEST_F(ReconnectTest, CatchesUpAfterAServerRestartWithoutLossOrDuplication) {
    start_server();
    const std::uint16_t fixed_port = port();
    config.port = fixed_port;

    Harness harness(options_for(fixed_port, "ada"));
    harness.call([&] {
        harness.client().join("general");
        harness.client().start();
    });
    ASSERT_TRUE(eventually([&] { return harness.joins() == 1; }));

    {
        auto writer = login("writer");
        writer.join("general");
        for (int i = 1; i <= 5; ++i) {
            writer.send(Publish{0, "general", "before " + std::to_string(i)});
        }
        for (int i = 1; i <= 5; ++i) {
            ASSERT_TRUE(writer.wait_for<Ack>().has_value());
        }
    }
    ASSERT_TRUE(eventually([&] { return harness.messages().size() == 5; }));

    server->stop();
    server.reset();
    ASSERT_TRUE(eventually([&] { return harness.state() != ChatClient::State::Online; }));

    start_server();

    // The writer may race the client's reconnect: both orders must work.
    {
        auto writer = login("writer");
        writer.join("general");
        for (int i = 1; i <= 5; ++i) {
            writer.send(Publish{0, "general", "after " + std::to_string(i)});
        }
        for (int i = 1; i <= 5; ++i) {
            ASSERT_TRUE(writer.wait_for<Ack>().has_value());
        }
    }

    ASSERT_TRUE(eventually([&] { return harness.messages().size() >= 10; }));
    const auto messages = harness.messages();
    ASSERT_EQ(messages.size(), 10U);
    for (std::size_t i = 0; i < messages.size(); ++i) {
        EXPECT_EQ(messages[i].seq, i + 1);
    }
    EXPECT_GE(harness.times_online(), 2);
}

TEST_F(ServerTest, ReportsMessagesThatAgedOutWhileOffline) {
    config.history_messages = 2;
    start_server();
    const std::uint16_t fixed_port = port();

    auto writer = login("writer");
    writer.join("general");
    writer.send(Publish{0, "general", "one"});
    ASSERT_TRUE(writer.wait_for<Ack>().has_value());

    // A client rejoining from seq 1, when history now starts at seq 4.
    for (int i = 0; i < 4; ++i) {
        writer.send(Publish{0, "general", "more"});
        ASSERT_TRUE(writer.wait_for<Ack>().has_value());
    }
    auto reader = TestClient::login(fixed_port, "reader");
    const auto joined = reader.join("general", 1);
    EXPECT_EQ(joined.first_seq, 4U);
    EXPECT_EQ(joined.head_seq, 5U);
}

#ifdef ORBIT_HAS_TLS

class TlsServerTest : public ServerTest {
protected:
    TlsServerTest() {
        config.tls_cert = cert.cert_path.string();
        config.tls_key = cert.key_path.string();
    }
    TempDir dir;
    orbit::testing::TestCertificate cert{dir.path()};
};

TEST_F(TlsServerTest, ClientsTalkOverTls) {
    start_server();
    auto options = options_for(port(), "ada");
    options.host = "localhost";
    options.tls = net::TlsContext::client(cert.cert_path.string(), true);

    Harness harness(options);
    harness.call([&] {
        harness.client().join("secure");
        harness.client().start();
    });
    ASSERT_TRUE(eventually([&] { return harness.joins() == 1; }));
    harness.call([&] { harness.client().publish("secure", "over tls"); });
    ASSERT_TRUE(eventually([&] { return harness.messages().size() == 1; }));
    EXPECT_EQ(harness.messages()[0].body, "over tls");
}

TEST_F(TlsServerTest, RefusesPlaintextClients) {
    start_server();
    TestClient plain(port());
    plain.send(Hello{kProtocolVersion, "ada", ""});
    EXPECT_TRUE(plain.wait_closed());
}

TEST_F(TlsServerTest, ClientRejectsUntrustedCertificate) {
    start_server();
    TempDir other_dir;
    const orbit::testing::TestCertificate other(other_dir.path());

    auto options = options_for(port(), "ada");
    options.host = "localhost";
    options.tls = net::TlsContext::client(other.cert_path.string(), true);
    options.reconnect = false;

    Harness harness(options);
    harness.call([&] { harness.client().start(); });
    EXPECT_TRUE(eventually([&] { return harness.state() == ChatClient::State::Stopped; }));
    EXPECT_NE(harness.detail().find("TLS"), std::string::npos);
}

#endif
