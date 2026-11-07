#include "net/connection.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <future>
#include <numeric>
#include <optional>
#include <thread>

#include "net/listener.h"

using namespace orbit::net;
using orbit::ByteBuffer;

namespace {

SharedBytes bytes_from(std::string_view text) {
    return std::make_shared<const std::vector<std::uint8_t>>(text.begin(), text.end());
}

// A socket pair with one end managed by a Connection on a live event loop and
// the other left as a plain blocking descriptor under the test's control.
class ConnectionFixture : public ::testing::Test {
protected:
    void SetUp() override {
        std::array<int, 2> fds{};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()), 0);
        m_local.reset(fds[0]);
        peer.reset(fds[1]);
#ifdef SO_NOSIGPIPE
        const int enabled = 1;
        ::setsockopt(peer.get(), SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
        m_thread = std::thread([this] { loop.run(); });
    }

    void TearDown() override {
        on_loop([this] { connection.reset(); });
        loop.stop();
        m_thread.join();
    }

    void open(Connection::Options options = {}) {
        on_loop([&] {
            Connection::Handlers handlers;
            handlers.on_data = [this](ByteBuffer& input) {
                const auto bytes = input.readable();
                received.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                input.consume(bytes.size());
                if (echo) {
                    connection->send(bytes_from(received));
                    received.clear();
                }
            };
            handlers.on_close = [this](CloseReason reason) {
                closed.set_value(reason);
                connection.reset();  // Destroying from on_close must be safe.
            };
            connection = std::make_unique<Connection>(loop, std::move(m_local), options,
                                                      std::move(handlers));
            connection->start();
        });
    }

    template <typename F>
    void on_loop(F&& work) {
        std::promise<void> done;
        loop.post([&] {
            work();
            done.set_value();
        });
        done.get_future().wait();
    }

    std::string read_peer(std::size_t count) {
        std::string data(count, '\0');
        std::size_t total = 0;
        while (total < count) {
            const ssize_t n = ::read(peer.get(), data.data() + total, count - total);
            if (n <= 0) {
                break;
            }
            total += static_cast<std::size_t>(n);
        }
        data.resize(total);
        return data;
    }

    CloseReason wait_closed() {
        auto future = closed.get_future();
        EXPECT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
        return future.get();
    }

    EventLoop loop;
    UniqueFd peer;
    std::unique_ptr<Connection> connection;
    std::string received;
    bool echo = false;
    std::promise<CloseReason> closed;

private:
    UniqueFd m_local;
    std::thread m_thread;
};

}  // namespace

TEST_F(ConnectionFixture, DeliversIncomingBytes) {
    echo = true;
    open();
    ASSERT_EQ(::write(peer.get(), "ping", 4), 4);
    EXPECT_EQ(read_peer(4), "ping");
}

TEST_F(ConnectionFixture, ReportsOrderlyPeerClose) {
    open();
    peer.reset();
    EXPECT_EQ(wait_closed(), CloseReason::PeerClosed);
}

TEST_F(ConnectionFixture, BatchesManySendsInOrder) {
    open();
    on_loop([&] {
        for (char c = 'a'; c <= 'z'; ++c) {
            connection->send(bytes_from(std::string(1, c)));
        }
    });
    EXPECT_EQ(read_peer(26), "abcdefghijklmnopqrstuvwxyz");
}

TEST_F(ConnectionFixture, CompletesWritesLargerThanTheSocketBuffer) {
    open();
    constexpr std::size_t kSize = 8 * 1024 * 1024;
    auto payload = std::make_shared<std::vector<std::uint8_t>>(kSize);
    std::iota(payload->begin(), payload->end(), std::uint8_t{0});
    on_loop([&] { connection->send(payload); });

    const std::string data = read_peer(kSize);
    ASSERT_EQ(data.size(), kSize);
    EXPECT_TRUE(std::equal(payload->begin(), payload->end(),
                           reinterpret_cast<const std::uint8_t*>(data.data())));
}

TEST_F(ConnectionFixture, DisconnectsConsumerThatExceedsTheQueueBound) {
    Connection::Options options;
    options.max_queued_bytes = 1024 * 1024;
    open(options);

    auto block = std::make_shared<const std::vector<std::uint8_t>>(256 * 1024, std::uint8_t{1});
    // The peer never reads, so the kernel buffer fills and the queue grows.
    std::atomic<bool> stop{false};
    std::thread feeder([&] {
        while (!stop) {
            loop.post([&] {
                if (connection) {
                    connection->send(block);
                }
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    EXPECT_EQ(wait_closed(), CloseReason::SlowConsumer);
    stop = true;
    feeder.join();
}

TEST_F(ConnectionFixture, CloseAfterFlushDeliversQueuedBytesThenHalfCloses) {
    open();
    on_loop([&] {
        connection->send(bytes_from("goodbye"));
        connection->close_after_flush();
    });
    EXPECT_EQ(read_peer(7), "goodbye");

    std::array<char, 8> extra{};
    EXPECT_EQ(::read(peer.get(), extra.data(), extra.size()), 0);  // EOF from the half-close.

    peer.reset();
    EXPECT_EQ(wait_closed(), CloseReason::Local);
}

TEST_F(ConnectionFixture, CloseAfterFlushIsBoundedWhenThePeerNeverCloses) {
    Connection::Options options;
    options.linger_ms = 50;
    open(options);
    on_loop([&] { connection->close_after_flush(); });
    EXPECT_EQ(wait_closed(), CloseReason::Local);
}

TEST_F(ConnectionFixture, CloseInsideDataHandlerIsSafe) {
    open();
    on_loop([&] {
        connection = nullptr;  // Replace the handler set up by open().
    });
    std::array<int, 2> fds{};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()), 0);
    UniqueFd local(fds[0]);
    peer.reset(fds[1]);

    on_loop([&] {
        Connection::Handlers handlers;
        handlers.on_data = [this](ByteBuffer& input) {
            input.clear();
            connection->close();
            connection->send(bytes_from("ignored"));  // Must be a harmless no-op.
        };
        handlers.on_close = [this](CloseReason reason) {
            closed.set_value(reason);
            connection.reset();
        };
        connection = std::make_unique<Connection>(loop, std::move(local), Connection::Options{},
                                                  std::move(handlers));
        connection->start();
    });

    ASSERT_EQ(::write(peer.get(), "x", 1), 1);
    EXPECT_EQ(wait_closed(), CloseReason::Local);
}

TEST_F(ConnectionFixture, WritingToADeadPeerClosesInsteadOfRaisingSigpipe) {
    open();
    peer.reset();
    on_loop([&] {
        if (connection) {
            connection->send(bytes_from("into the void"));
        }
    });
    const auto reason = wait_closed();
    EXPECT_TRUE(reason == CloseReason::PeerClosed || reason == CloseReason::IoError);
}

TEST(TcpConnect, ConnectsThroughListener) {
    EventLoop loop;
    std::promise<void> accepted;
    std::promise<void> connected;
    std::vector<UniqueFd> sockets;
    Listener listener(loop, "127.0.0.1", 0, 16, [&](UniqueFd fd) {
        sockets.push_back(std::move(fd));
        accepted.set_value();
    });
    ASSERT_NE(listener.port(), 0);

    std::thread thread([&] { loop.run(); });
    std::unique_ptr<Connection> connection;
    loop.post([&] {
        listener.start();
        Connection::Handlers handlers;
        handlers.on_connected = [&] { connected.set_value(); };
        handlers.on_data = [](ByteBuffer&) {};
        handlers.on_close = [](CloseReason) {};
        connection = std::make_unique<Connection>(loop, connect_tcp("127.0.0.1", listener.port()),
                                                  Connection::Options{}, std::move(handlers));
        connection->start_connecting();
    });

    EXPECT_EQ(connected.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_EQ(accepted.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);

    std::promise<void> cleaned;
    loop.post([&] {
        connection.reset();
        listener.stop();
        cleaned.set_value();
    });
    cleaned.get_future().wait();
    loop.stop();
    thread.join();
}

TEST(TcpConnect, ReportsRefusedConnection) {
    // Reserve a port, then release it so nothing is listening there.
    std::uint16_t port = 0;
    {
        UniqueFd listener = listen_tcp("127.0.0.1", 0, 1);
        port = local_port(listener.get());
    }

    EventLoop loop;
    std::promise<CloseReason> closed;
    std::thread thread([&] { loop.run(); });
    std::unique_ptr<Connection> connection;
    loop.post([&] {
        Connection::Handlers handlers;
        handlers.on_data = [](ByteBuffer&) {};
        handlers.on_close = [&](CloseReason reason) { closed.set_value(reason); };
        connection = std::make_unique<Connection>(loop, connect_tcp("127.0.0.1", port),
                                                  Connection::Options{}, std::move(handlers));
        connection->start_connecting();
    });

    auto future = closed.get_future();
    ASSERT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_EQ(future.get(), CloseReason::ConnectFailed);

    std::promise<void> cleaned;
    loop.post([&] {
        connection.reset();
        cleaned.set_value();
    });
    cleaned.get_future().wait();
    loop.stop();
    thread.join();
}
