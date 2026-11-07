#include "server/shard.h"

#include <algorithm>
#include <format>
#include <utility>

#include "common/clock.h"
#include "common/log.h"
#ifdef ORBIT_HAS_TLS
#include "net/tls.h"
#endif

namespace orbit::server {
using namespace protocol;

Shard::Shard(std::size_t index, ServerContext& context) : m_index(index), m_context(context) {}

Shard::~Shard() = default;

void Shard::start() {
    m_thread = std::thread([this] { m_loop.run(); });
}

void Shard::join_thread() {
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

Shard& Shard::owner_of(const std::string& room) {
    return *m_context.shards[room_shard_index(room, m_context.shards.size())];
}

Shard& Shard::shard_of(SessionId session) {
    return *m_context.shards[session_shard(session)];
}

void Shard::adopt(net::UniqueFd fd) {
    // std::function needs a copyable callable, hence the shared holder.
    auto holder = std::make_shared<net::UniqueFd>(std::move(fd));
    m_loop.post([this, holder] {
        if (m_shutting_down) {
            m_context.metrics.connections_rejected.add();
            return;
        }
        std::unique_ptr<net::SecureChannel> channel;
#ifdef ORBIT_HAS_TLS
        if (m_context.tls != nullptr) {
            channel = m_context.tls->create_channel();
        }
#endif
        const SessionId id = make_session_id(m_index, m_next_session++);
        auto session = std::make_unique<Session>(*this, m_context, id, std::move(*holder),
                                                 std::move(channel));
        Session* raw = session.get();
        m_sessions.emplace(id, std::move(session));
        m_context.metrics.connections_active.add();
        m_context.metrics.connections_accepted.add();
        raw->start();
    });
}

void Shard::begin_shutdown() {
    m_loop.post([this] {
        m_shutting_down = true;
        for (auto& entry : m_sessions) {
            entry.second->terminate(ErrorCode::ServerShutdown, "server is shutting down");
        }
    });
}

void Shard::session_closed(SessionId id) {
    const auto found = m_sessions.find(id);
    if (found == m_sessions.end()) {
        return;
    }
    // Forget the session before telling room owners. An owner on this shard runs
    // inline and must not find a half-closed session to reply to.
    const std::unique_ptr<Session> session = std::move(found->second);
    m_sessions.erase(found);
    m_context.metrics.connections_active.sub();

    for (const auto& [room_id, name] : session->rooms()) {
        unsubscribe(room_id, session.get());
        Shard& owner = owner_of(name);
        run_on(owner, [&owner, id, room = name] { owner.on_leave(id, room); });
    }
}

// ---- Routing: runs on the session's shard, hands off to the room's owner. ----

void Shard::join_room(SessionId session, const std::string& nick, std::string room,
                      std::uint64_t since_seq) {
    Shard& owner = owner_of(room);
    run_on(owner, [&owner, session, nick, room = std::move(room), since_seq] {
        owner.on_join(session, nick, room, since_seq);
    });
}

void Shard::leave_room(SessionId session, std::string room) {
    Shard& owner = owner_of(room);
    run_on(owner, [&owner, session, room = std::move(room)] { owner.on_leave(session, room); });
}

void Shard::who(SessionId session, std::string room) {
    Shard& owner = owner_of(room);
    run_on(owner, [&owner, session, room = std::move(room)] { owner.on_who(session, room); });
}

void Shard::publish(PublishRequest&& request) {
    Shard& owner = owner_of(request.room);
    // Load shedding: refuse at the door, rather than let an overwhelmed shard's
    // queue grow until the process runs out of memory.
    if (owner.m_loop.queue_depth() >= m_context.config.max_shard_queue) {
        m_context.metrics.overloaded.add();
        deliver_one(request.sender, make_frame(Error{request.correlation, ErrorCode::Overloaded,
                                                     "room shard is overloaded"}));
        return;
    }
    if (&owner == this) {
        on_publish(std::move(request));
    } else {
        // std::function needs a copyable callable, hence the shared holder.
        auto held = std::make_shared<PublishRequest>(std::move(request));
        owner.m_loop.post([&owner, held] { owner.on_publish(std::move(*held)); });
    }
}

void Shard::send_to(SessionId session, Frame frame) {
    Shard& target = shard_of(session);
    run_on(target, [&target, session, frame = std::move(frame)] {
        target.deliver_one(session, frame);
    });
}

// ---- Room role: runs on the owning shard, which is the room's only writer. ----

Room* Shard::find_room(const std::string& name) {
    const auto found = m_rooms.find(name);
    return found == m_rooms.end() ? nullptr : found->second.get();
}

Room& Shard::get_or_create_room(const std::string& name) {
    auto& slot = m_rooms[name];
    if (!slot) {
        const ServerConfig& config = m_context.config;
        slot = std::make_unique<Room>(name, m_context.next_room_id.fetch_add(1),
                                      m_context.shards.size(),
                                      Room::Limits{config.history_messages, config.history_bytes});
        m_context.metrics.rooms_active.add();
    }
    return *slot;
}

void Shard::restore_message(const storage::WalRecord& record) {
    Room& room = get_or_create_room(record.room);
    room.restore_head(record.seq);
    room.commit(record.seq, make_frame(Message{record.room, record.seq, record.timestamp_ms,
                                               record.nick, record.body}));
    m_context.directory.set_member_count(record.room, 0);
}

void Shard::restore_head(const std::string& name, std::uint64_t head) {
    get_or_create_room(name).restore_head(head);
    m_context.directory.set_member_count(name, 0);
}

void Shard::on_join(SessionId session, const std::string& nick, const std::string& name,
                    std::uint64_t since_seq) {
    Shard& home = shard_of(session);
    if (find_room(name) == nullptr && m_context.directory.size() >= m_context.config.max_rooms) {
        run_on(home, [&home, session, error = make_frame(Error{0, ErrorCode::Overloaded,
                                                               "room limit reached"})] {
            home.on_join_refused(session, error);
        });
        return;
    }

    Room& room = get_or_create_room(name);
    const bool newly_joined = room.add_member(session, nick);
    const auto member_count = static_cast<std::uint32_t>(room.member_count());

    Frame joined = make_frame(Joined{name, room.head_seq(), room.first_seq(), member_count});
    std::vector<Frame> history = room.history_after(since_seq);

    // The subscription, the replay and everything fanned out after this line reach
    // the session's shard in order through one queue. That FIFO hop is what makes
    // the hand-over from history to live stream gap-free.
    run_on(home, [&home, session, id = room.id(), name, joined = std::move(joined),
                  history = std::move(history)] {
        home.on_joined(session, id, name, joined, history);
    });

    if (newly_joined) {
        m_context.directory.set_member_count(name, member_count);
        fan_out(room, make_frame(Presence{name, nick, true, wall_clock_ms()}));
    }
}

void Shard::on_leave(SessionId session, const std::string& name) {
    Room* room = find_room(name);
    if (room == nullptr) {
        return;
    }
    // The nickname is carried by the room, since the session may already be gone.
    std::string nick;
    if (room->has_member(session)) {
        nick = room->nick_of(session);
    }
    if (!room->remove_member(session)) {
        return;
    }

    Shard& home = shard_of(session);
    run_on(home, [&home, session, id = room->id(), left = make_frame(Left{name})] {
        home.on_left(session, id, left);
    });

    m_context.directory.set_member_count(name, static_cast<std::uint32_t>(room->member_count()));
    fan_out(*room, make_frame(Presence{name, nick, false, wall_clock_ms()}));
}

void Shard::on_who(SessionId session, const std::string& name) {
    Room* room = find_room(name);
    Frame reply = room != nullptr
                      ? make_frame(WhoReply{name, room->nicks()})
                      : make_frame(Error{0, ErrorCode::NotInRoom, name});
    send_to(session, std::move(reply));
}

void Shard::on_publish(PublishRequest&& request) {
    Room* room = find_room(request.room);
    if (room == nullptr || !room->has_member(request.sender)) {
        send_to(request.sender,
                make_frame(Error{request.correlation, ErrorCode::NotInRoom, request.room}));
        return;
    }

    const std::uint64_t seq = room->allocate_seq();
    const std::uint64_t now = wall_clock_ms();
    Frame frame = make_frame(Message{request.room, seq, now, request.nick, request.body});
    Frame ack = make_frame(Ack{request.correlation, seq, now});

    storage::Wal* wal = m_context.wal;
    if (wal == nullptr) {
        finish_publish(*room, seq, frame, request.sender, ack, request.received_ns);
        return;
    }

    storage::WalRecord record{std::move(request.room), seq, now, std::move(request.nick),
                              std::move(request.body)};
    const SessionId sender = request.sender;
    const std::uint64_t received_ns = request.received_ns;

    storage::Wal::Completion completion;
    const bool wait_for_disk = m_context.config.sync_policy == storage::SyncPolicy::Always;
    if (wait_for_disk) {
        // Nobody sees the message until it is on disk, so a crash can never
        // retract something a client has already read. The writer completes in
        // log order and the posts are FIFO, so room order is preserved. Rooms
        // are never destroyed, so the pointer stays valid.
        completion = [this, room, seq, frame, ack, sender, received_ns](bool durable) {
            m_loop.post([this, room, seq, frame, ack, sender, received_ns, durable] {
                if (durable) {
                    finish_publish(*room, seq, frame, sender, ack, received_ns);
                } else {
                    send_to(sender, make_frame(Error{0, ErrorCode::StorageFailure, {}}));
                }
            });
        };
    }

    if (!wal->append(std::move(record), std::move(completion))) {
        room->release_seq();
        m_context.metrics.overloaded.add();
        const auto code = wal->failed() ? ErrorCode::StorageFailure : ErrorCode::Overloaded;
        send_to(sender, make_frame(Error{0, code, "message not stored"}));
        return;
    }
    if (!wait_for_disk) {
        finish_publish(*room, seq, frame, sender, ack, received_ns);
    }
}

void Shard::finish_publish(Room& room, std::uint64_t seq, const Frame& frame, SessionId sender,
                           const Frame& ack, std::uint64_t received_ns) {
    room.commit(seq, frame);
    fan_out(room, frame);
    send_to(sender, ack);
    m_context.metrics.messages_published.add();
    m_context.metrics.publish_latency.observe(static_cast<double>(monotonic_ns() - received_ns) /
                                              1e9);
}

void Shard::fan_out(const Room& room, const Frame& frame) {
    const auto& per_shard = room.members_per_shard();
    for (std::size_t i = 0; i < per_shard.size(); ++i) {
        if (per_shard[i] == 0) {
            continue;
        }
        Shard& target = *m_context.shards[i];
        run_on(target, [&target, id = room.id(), frame] { target.deliver(id, frame); });
    }
}

// ---- Session role: runs on the shard that holds the session. ----

void Shard::on_joined(SessionId session_id, std::uint32_t room_id, const std::string& name,
                      const Frame& joined, const std::vector<Frame>& history) {
    const auto found = m_sessions.find(session_id);
    if (found == m_sessions.end()) {
        // The client vanished while the join was in flight. Undo it, or the room
        // would keep a ghost member forever.
        Shard& owner = owner_of(name);
        run_on(owner, [&owner, session_id, name] { owner.on_leave(session_id, name); });
        return;
    }
    Session& session = *found->second;
    session.room_request_finished();
    if (!session.is_subscribed(room_id)) {
        session.add_room(room_id, name);
        m_subscribers[room_id].push_back(&session);
    }
    session.send(joined);
    for (const Frame& frame : history) {
        session.send(frame);
    }
}

void Shard::on_join_refused(SessionId session_id, const Frame& error) {
    const auto found = m_sessions.find(session_id);
    if (found != m_sessions.end()) {
        found->second->room_request_finished();
        found->second->send(error);
    }
}

void Shard::on_left(SessionId session_id, std::uint32_t room_id, const Frame& left) {
    const auto found = m_sessions.find(session_id);
    if (found == m_sessions.end()) {
        return;
    }
    Session& session = *found->second;
    unsubscribe(room_id, &session);
    session.remove_room(room_id);
    session.send(left);
}

void Shard::unsubscribe(std::uint32_t room_id, Session* session) {
    const auto found = m_subscribers.find(room_id);
    if (found == m_subscribers.end()) {
        return;
    }
    auto& subscribers = found->second;
    const auto position = std::find(subscribers.begin(), subscribers.end(), session);
    if (position != subscribers.end()) {
        *position = subscribers.back();
        subscribers.pop_back();
    }
    if (subscribers.empty()) {
        m_subscribers.erase(found);
    }
}

void Shard::deliver(std::uint32_t room_id, const Frame& frame) {
    const auto found = m_subscribers.find(room_id);
    if (found == m_subscribers.end()) {
        return;
    }
    for (Session* session : found->second) {
        session->send(frame);
    }
    m_context.metrics.messages_delivered.add(found->second.size());
}

void Shard::deliver_one(SessionId session_id, const Frame& frame) {
    const auto found = m_sessions.find(session_id);
    if (found != m_sessions.end()) {
        found->second->send(frame);
    }
}

}  // namespace orbit::server
