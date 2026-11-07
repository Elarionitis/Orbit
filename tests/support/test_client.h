#pragma once

#include <gtest/gtest.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "common/byte_buffer.h"
#include "common/clock.h"
#include "net/socket.h"
#include "protocol/messages.h"

namespace orbit::testing {

// A deliberately plain, blocking client: it shares nothing with the server's
// reactor, so it can act as an independent witness of what is on the wire.
class TestClient {
public:
    explicit TestClient(std::uint16_t port) {
        m_fd = net::connect_tcp("127.0.0.1", port);
        pollfd waiting{m_fd.get(), POLLOUT, 0};
        ::poll(&waiting, 1, 5000);
    }

    // Connects and completes the handshake.
    static TestClient login(std::uint16_t port, const std::string& nick,
                            const std::string& token = {}) {
        TestClient client(port);
        client.send(protocol::Hello{protocol::kProtocolVersion, nick, token});
        client.expect<protocol::Welcome>();
        return client;
    }

    void send(const protocol::AnyMessage& message) { send_raw(protocol::encode(message)); }

    void send_raw(const std::vector<std::uint8_t>& bytes) {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const ssize_t sent = ::send(m_fd.get(), bytes.data() + offset, bytes.size() - offset,
                                        net::send_flags());
            if (sent > 0) {
                offset += static_cast<std::size_t>(sent);
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                pollfd waiting{m_fd.get(), POLLOUT, 0};
                ::poll(&waiting, 1, 1000);
            } else {
                return;  // The server hung up; tests observe that through receive().
            }
        }
    }

    // The next message, or nullopt on timeout or disconnect.
    std::optional<protocol::AnyMessage> receive(int timeout_ms = 5000) {
        const std::uint64_t deadline = monotonic_ms() + static_cast<std::uint64_t>(timeout_ms);
        for (;;) {
            const auto frame =
                protocol::extract_frame(m_input.readable(), 16 * 1024 * 1024);
            if (frame.status == protocol::ExtractStatus::Complete) {
                auto message = protocol::decode(frame.body);
                m_input.consume(frame.consumed);
                return message;
            }
            const std::uint64_t now = monotonic_ms();
            if (now >= deadline || m_closed) {
                return std::nullopt;
            }
            pollfd waiting{m_fd.get(), POLLIN, 0};
            if (::poll(&waiting, 1, static_cast<int>(deadline - now)) <= 0) {
                return std::nullopt;
            }
            auto target = m_input.prepare(64 * 1024);
            const ssize_t count = ::recv(m_fd.get(), target.data(), target.size(), 0);
            if (count > 0) {
                m_input.commit(static_cast<std::size_t>(count));
            } else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                m_closed = true;
            }
        }
    }

    // Waits for a T, failing the test if the next message is anything else.
    template <typename T>
    T expect(int timeout_ms = 5000) {
        auto message = receive(timeout_ms);
        if (!message.has_value()) {
            ADD_FAILURE() << "no message received";
            return T{};
        }
        if (auto* typed = std::get_if<T>(&*message)) {
            return std::move(*typed);
        }
        if (const auto* error = std::get_if<protocol::Error>(&*message)) {
            ADD_FAILURE() << "unexpected error: " << protocol::error_name(error->code) << " "
                          << error->detail;
        } else {
            ADD_FAILURE() << "unexpected message, variant index " << message->index();
        }
        return T{};
    }

    // Waits for a T, skipping anything else.
    template <typename T>
    std::optional<T> wait_for(int timeout_ms = 5000) {
        const std::uint64_t deadline = monotonic_ms() + static_cast<std::uint64_t>(timeout_ms);
        for (;;) {
            const std::uint64_t now = monotonic_ms();
            if (now >= deadline) {
                return std::nullopt;
            }
            auto message = receive(static_cast<int>(deadline - now));
            if (!message.has_value()) {
                return std::nullopt;
            }
            if (auto* typed = std::get_if<T>(&*message)) {
                return std::move(*typed);
            }
        }
    }

    protocol::Joined join(const std::string& room, std::uint64_t since = 0) {
        send(protocol::Join{room, since});
        auto joined = wait_for<protocol::Joined>();
        EXPECT_TRUE(joined.has_value()) << "join failed for " << room;
        return joined.value_or(protocol::Joined{});
    }

    // True once the server has closed the connection.
    bool wait_closed(int timeout_ms = 5000) {
        const std::uint64_t deadline = monotonic_ms() + static_cast<std::uint64_t>(timeout_ms);
        while (monotonic_ms() < deadline) {
            if (!receive(50).has_value() && m_closed) {
                return true;
            }
        }
        return false;
    }

    void disconnect() { m_fd.reset(); }

private:
    net::UniqueFd m_fd;
    ByteBuffer m_input;
    bool m_closed = false;
};

}  // namespace orbit::testing
