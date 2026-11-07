#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "common/timer_wheel.h"
#include "common/token_bucket.h"
#include "net/connection.h"
#include "protocol/messages.h"
#include "server/directory.h"
#include "server/server_context.h"

namespace orbit::server {

// One client connection, from accept to close. Parses frames, enforces the
// protocol state machine and per-client limits, and turns each request into work
// for the shard that owns the room concerned. Runs only on its own shard's thread.
class Session {
public:
    Session(Shard& shard, ServerContext& context, SessionId id, net::UniqueFd fd,
            std::unique_ptr<net::SecureChannel> channel);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void start();

    SessionId id() const { return m_id; }
    const std::string& nick() const { return m_nick; }

    void send(const protocol::Frame& frame) { m_connection->send(frame); }

    // Sends a final error, then closes once it has been delivered.
    void terminate(protocol::ErrorCode code, std::string detail);

    const std::unordered_map<std::uint32_t, std::string>& rooms() const { return m_rooms; }
    bool is_subscribed(std::uint32_t room_id) const { return m_rooms.contains(room_id); }
    void add_room(std::uint32_t room_id, const std::string& name) { m_rooms[room_id] = name; }
    void remove_room(std::uint32_t room_id) { m_rooms.erase(room_id); }

    void room_request_finished() {
        if (m_pending_joins > 0) {
            --m_pending_joins;
        }
    }

private:
    enum class State { AwaitingHello, Active };

    void on_data(ByteBuffer& input);
    void on_close(net::CloseReason reason);
    void dispatch(protocol::AnyMessage&& message);

    void handle(protocol::Hello&& hello);
    void handle(protocol::Join&& join);
    void handle(protocol::Leave&& leave);
    void handle(protocol::Publish&& publish);
    void handle(protocol::Direct&& direct);
    void handle(protocol::Who&& who);
    void handle(protocol::ListRooms&&);
    void handle(protocol::Ping&& ping);

    void reply_error(protocol::ErrorCode code, std::uint64_t correlation, std::string detail = {});
    void protocol_violation(std::string detail);
    bool admit_message(std::uint64_t correlation);
    bool validate_body(const std::string& body, std::uint64_t correlation);

    void schedule_idle_check(std::uint64_t delay_ms);
    void on_idle_check();

    Shard& m_shard;
    ServerContext& m_context;
    SessionId m_id;
    std::unique_ptr<net::Connection> m_connection;
    std::string m_peer;

    State m_state = State::AwaitingHello;
    std::string m_nick;
    bool m_counted_as_session = false;
    std::unordered_map<std::uint32_t, std::string> m_rooms;
    std::size_t m_pending_joins = 0;

    TokenBucket m_rate_limiter;
    std::uint32_t m_consecutive_rate_limits = 0;
    std::uint64_t m_last_activity_ms = 0;
    TimerWheel::TimerId m_idle_timer = 0;
};

}  // namespace orbit::server
