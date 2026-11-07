#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "common/timer_wheel.h"
#include "net/connection.h"
#include "net/event_loop.h"
#include "protocol/messages.h"

namespace orbit::net {
class TlsContext;
}

namespace orbit::client {

// A chat client that stays connected. It runs on an event loop and is not
// thread-safe: call it from the loop thread, and callbacks arrive there too.
//
// After a lost connection it reconnects with exponential backoff and full jitter,
// so that thousands of clients dropped by one server restart do not all return at
// the same instant. It rejoins every room from the last sequence number it saw,
// and the server replays what was missed.
class ChatClient {
public:
    enum class State {
        Idle,
        Connecting,  // TCP, TLS and Hello in progress.
        Online,
        Backoff,  // Waiting to retry.
        Stopped,  // Will not reconnect.
    };

    struct Options {
        std::string host = "127.0.0.1";
        std::uint16_t port = 8080;
        std::string nick;
        std::string token;
        std::shared_ptr<net::TlsContext> tls;
        bool reconnect = true;
        std::uint64_t connect_timeout_ms = 5000;
        std::uint64_t min_backoff_ms = 250;
        std::uint64_t max_backoff_ms = 30'000;
    };

    struct Events {
        std::function<void(State, const std::string& detail)> on_state;
        // `missed` counts messages that aged out of history while offline.
        std::function<void(const protocol::Joined&, std::uint64_t missed)> on_joined;
        std::function<void(const protocol::Left&)> on_left;
        std::function<void(const protocol::Message&, bool history)> on_message;
        std::function<void(const protocol::Presence&)> on_presence;
        std::function<void(const protocol::DirectMessage&)> on_direct;
        std::function<void(const protocol::WhoReply&)> on_who;
        std::function<void(const protocol::RoomList&)> on_rooms;
        std::function<void(const protocol::Error&)> on_error;
    };

    ChatClient(net::EventLoop& loop, Options options, Events events);
    ~ChatClient();

    ChatClient(const ChatClient&) = delete;
    ChatClient& operator=(const ChatClient&) = delete;

    void start();
    void stop();

    State state() const { return m_state; }

    void join(const std::string& room);
    void leave(const std::string& room);
    bool publish(const std::string& room, const std::string& body);
    bool direct(const std::string& nick, const std::string& body);
    void who(const std::string& room);
    void list_rooms();

private:
    struct RoomPosition {
        std::uint64_t last_seq = 0;
        std::uint64_t history_until = 0;  // Messages up to here are replayed history.
    };

    void connect();
    void connect_to_next_address();
    void schedule_retry(const std::string& detail);
    void set_state(State state, const std::string& detail = {});
    void on_connected();
    void on_data(ByteBuffer& input);
    void on_closed(net::CloseReason reason);
    void handle(const protocol::AnyMessage& message);
    void on_welcome(const protocol::Welcome& welcome);
    void on_server_error(const protocol::Error& error);
    void send(const protocol::AnyMessage& message);
    void cancel_timers();
    void heartbeat();

    net::EventLoop& m_loop;
    Options m_options;
    Events m_events;
    State m_state = State::Idle;

    std::unique_ptr<net::Connection> m_connection;
    std::vector<net::SocketAddress> m_addresses;
    std::size_t m_next_address = 0;
    std::map<std::string, RoomPosition> m_rooms;
    std::string m_close_detail;

    std::uint32_t m_attempt = 0;
    bool m_ever_online = false;
    std::uint64_t m_next_correlation = 1;
    std::uint64_t m_heartbeat_ms = 0;
    std::uint64_t m_last_received_ms = 0;
    std::uint32_t m_max_frame_size = 16 * 1024 * 1024;

    TimerWheel::TimerId m_retry_timer = 0;
    TimerWheel::TimerId m_connect_timer = 0;
    TimerWheel::TimerId m_heartbeat_timer = 0;

    std::mt19937_64 m_random{std::random_device{}()};
};

}  // namespace orbit::client
