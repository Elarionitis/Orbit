#include "server/room.h"

#include <algorithm>
#include <utility>

namespace orbit::server {

Room::Room(std::string name, std::uint32_t id, std::size_t shard_count, Limits limits)
    : m_name(std::move(name)), m_id(id), m_limits(limits), m_members_per_shard(shard_count, 0) {}

bool Room::add_member(SessionId session, const std::string& nick) {
    if (!m_members.try_emplace(session, Member{nick}).second) {
        return false;
    }
    ++m_members_per_shard[session_shard(session)];
    return true;
}

bool Room::remove_member(SessionId session) {
    if (m_members.erase(session) == 0) {
        return false;
    }
    --m_members_per_shard[session_shard(session)];
    return true;
}

std::vector<std::string> Room::nicks() const {
    std::vector<std::string> names;
    names.reserve(m_members.size());
    for (const auto& [session, member] : m_members) {
        names.push_back(member.nick);
    }
    std::sort(names.begin(), names.end());
    return names;
}

void Room::commit(std::uint64_t seq, protocol::Frame frame) {
    m_head_seq = seq;
    if (m_limits.history_messages == 0) {
        return;
    }
    m_history_bytes += frame->size();
    m_history.push_back(Retained{seq, std::move(frame)});
    while (m_history.size() > m_limits.history_messages ||
           (m_history_bytes > m_limits.history_bytes && m_history.size() > 1)) {
        m_history_bytes -= m_history.front().frame->size();
        m_history.pop_front();
    }
}

std::uint64_t Room::first_seq() const {
    return m_history.empty() ? m_head_seq + 1 : m_history.front().seq;
}

std::vector<protocol::Frame> Room::history_after(std::uint64_t since_seq) const {
    const auto first = std::upper_bound(
        m_history.begin(), m_history.end(), since_seq,
        [](std::uint64_t seq, const Retained& retained) { return seq < retained.seq; });

    std::vector<protocol::Frame> frames;
    frames.reserve(static_cast<std::size_t>(m_history.end() - first));
    for (auto entry = first; entry != m_history.end(); ++entry) {
        frames.push_back(entry->frame);
    }
    return frames;
}

void Room::restore_head(std::uint64_t head) {
    m_head_seq = std::max(m_head_seq, head);
    m_allocated_seq = m_head_seq;
}

}  // namespace orbit::server
