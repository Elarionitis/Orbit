#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "protocol/messages.h"
#include "server/directory.h"

namespace orbit::server {

// State of one room. Lives on exactly one shard and is only touched from that
// shard's thread, so it needs no locks: the room is the single writer of its own
// sequence, which is what gives every member the same message order.
class Room {
public:
    struct Limits {
        std::size_t history_messages;
        std::size_t history_bytes;
    };

    Room(std::string name, std::uint32_t id, std::size_t shard_count, Limits limits);

    const std::string& name() const { return m_name; }
    std::uint32_t id() const { return m_id; }

    bool add_member(SessionId session, const std::string& nick);
    bool remove_member(SessionId session);
    bool has_member(SessionId session) const { return m_members.contains(session); }
    std::size_t member_count() const { return m_members.size(); }
    std::vector<std::string> nicks() const;
    const std::string& nick_of(SessionId session) const { return m_members.at(session).nick; }

    // Number of this room's members on each shard. Fan-out skips shards at zero.
    const std::vector<std::uint32_t>& members_per_shard() const { return m_members_per_shard; }

    // A sequence number is allocated first and committed once the message is
    // durable, so between the two the head lags behind what has been allocated.
    std::uint64_t allocate_seq() { return ++m_allocated_seq; }
    void release_seq() { --m_allocated_seq; }
    void commit(std::uint64_t seq, protocol::Frame frame);

    std::uint64_t head_seq() const { return m_head_seq; }
    std::uint64_t first_seq() const;

    // Retained frames newer than `since_seq`, oldest first. A `since_seq` of zero
    // means the client has no position yet, and gets everything retained.
    std::vector<protocol::Frame> history_after(std::uint64_t since_seq) const;

    // Fast-forwards the sequence during recovery, which may find a head higher
    // than any retained message.
    void restore_head(std::uint64_t head);

private:
    struct Retained {
        std::uint64_t seq;
        protocol::Frame frame;
    };
    struct Member {
        std::string nick;
    };

    std::string m_name;
    std::uint32_t m_id;
    Limits m_limits;
    std::uint64_t m_allocated_seq = 0;
    std::uint64_t m_head_seq = 0;
    std::unordered_map<SessionId, Member> m_members;
    std::vector<std::uint32_t> m_members_per_shard;
    std::deque<Retained> m_history;
    std::size_t m_history_bytes = 0;
};

}  // namespace orbit::server
