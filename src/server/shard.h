#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "net/event_loop.h"
#include "net/socket.h"
#include "protocol/messages.h"
#include "server/room.h"
#include "server/server_context.h"
#include "server/session.h"

namespace orbit::server {

struct PublishRequest {
    SessionId sender;
    std::string nick;
    std::uint64_t correlation;
    std::string room;
    std::string body;
    std::uint64_t received_ns;
};

// One event-loop thread, in two roles at once:
//
//  * Session role: owns a set of connections and the local table of which of them
//    subscribe to which room.
//  * Room role: owns the rooms that hash to it, as their single writer.
//
// A published message therefore moves  ingress shard -> room owner -> egress
// shards. Each hop is a FIFO task queue, so with exactly one writer per room,
// every member of a room sees the same order with no lock on the path. The same
// queues carry subscribe and unsubscribe, which makes joining atomic against the
// stream: a member gets a gap-free, duplicate-free history followed by live
// messages, with no coordination beyond ordering.
class Shard {
public:
    Shard(std::size_t index, ServerContext& context);
    ~Shard();

    Shard(const Shard&) = delete;
    Shard& operator=(const Shard&) = delete;

    net::EventLoop& loop() { return m_loop; }
    std::size_t index() const { return m_index; }

    void start();
    void join_thread();

    // Before start() only: rebuilds a room from the log.
    void restore_message(const storage::WalRecord& record);
    void restore_head(const std::string& room, std::uint64_t head);

    // Thread-safe: moves an accepted socket onto this shard.
    void adopt(net::UniqueFd fd);

    // Thread-safe: tells every session the server is going down.
    void begin_shutdown();

    // Called from sessions on this shard's thread. Each routes to the owning shard.
    void join_room(SessionId session, const std::string& nick, std::string room,
                   std::uint64_t since_seq);
    void leave_room(SessionId session, std::string room);
    void publish(PublishRequest&& request);
    void who(SessionId session, std::string room);
    void send_to(SessionId session, protocol::Frame frame);
    void session_closed(SessionId session);

private:
    Shard& owner_of(const std::string& room);
    Shard& shard_of(SessionId session);

    // Runs a task on `target`: inline when it is this shard, queued otherwise.
    // Inline is safe because every path within a shard keeps the same FIFO order.
    template <typename F>
    void run_on(Shard& target, F&& task) {
        if (&target == this) {
            task();
        } else {
            target.m_loop.post(std::forward<F>(task));
        }
    }

    // Room role. Runs on the owning shard.
    Room* find_room(const std::string& name);
    Room& get_or_create_room(const std::string& name);
    void on_join(SessionId session, const std::string& nick, const std::string& name,
                 std::uint64_t since_seq);
    void on_leave(SessionId session, const std::string& name);
    void on_publish(PublishRequest&& request);
    void on_who(SessionId session, const std::string& name);
    void finish_publish(Room& room, std::uint64_t seq, const protocol::Frame& frame,
                        SessionId sender, const protocol::Frame& ack, std::uint64_t received_ns);
    void fan_out(const Room& room, const protocol::Frame& frame);

    // Session role. Runs on the session's shard.
    void on_joined(SessionId session, std::uint32_t room_id, const std::string& name,
                   const protocol::Frame& joined, const std::vector<protocol::Frame>& history);
    void on_left(SessionId session, std::uint32_t room_id, const protocol::Frame& left);
    void on_join_refused(SessionId session, const protocol::Frame& error);
    void deliver(std::uint32_t room_id, const protocol::Frame& frame);
    void deliver_one(SessionId session, const protocol::Frame& frame);
    void unsubscribe(std::uint32_t room_id, Session* session);

    std::size_t m_index;
    ServerContext& m_context;
    net::EventLoop m_loop;
    std::thread m_thread;
    bool m_shutting_down = false;

    std::uint64_t m_next_session = 1;
    std::unordered_map<SessionId, std::unique_ptr<Session>> m_sessions;
    std::unordered_map<std::uint32_t, std::vector<Session*>> m_subscribers;

    std::unordered_map<std::string, std::unique_ptr<Room>> m_rooms;
};

}  // namespace orbit::server
