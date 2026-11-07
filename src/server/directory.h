#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "protocol/messages.h"

namespace orbit::server {

using SessionId = std::uint64_t;

// The shard that owns a session is encoded in its id, so routing a reply to any
// session takes no lookup and no shared state.
inline constexpr int kSessionShardShift = 48;

inline SessionId make_session_id(std::size_t shard, std::uint64_t counter) {
    return static_cast<SessionId>(shard) << kSessionShardShift | counter;
}
inline std::size_t session_shard(SessionId id) {
    return static_cast<std::size_t>(id >> kSessionShardShift);
}

// FNV-1a. Room placement only has to be stable within a process and spread names
// evenly across shards.
inline std::size_t room_shard_index(std::string_view room, std::size_t shard_count) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const char c : room) {
        hash = (hash ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ULL;
    }
    return static_cast<std::size_t>(hash % shard_count);
}

// Maps nicknames to sessions, across all shards. Nicknames are unique ignoring
// case, so "Ada" cannot pose as "ada". This is the one piece of shared mutable
// state, and it stays off the message hot path: it is touched at login, logout
// and for direct messages, never for room traffic.
class NickRegistry {
public:
    bool try_claim(std::string_view nick, SessionId session) {
        const std::unique_lock lock(m_mutex);
        return m_sessions.try_emplace(fold(nick), session).second;
    }

    void release(std::string_view nick, SessionId session) {
        const std::unique_lock lock(m_mutex);
        const auto found = m_sessions.find(fold(nick));
        if (found != m_sessions.end() && found->second == session) {
            m_sessions.erase(found);
        }
    }

    std::optional<SessionId> find(std::string_view nick) const {
        const std::shared_lock lock(m_mutex);
        const auto found = m_sessions.find(fold(nick));
        if (found == m_sessions.end()) {
            return std::nullopt;
        }
        return found->second;
    }

private:
    static std::string fold(std::string_view nick) {
        std::string folded(nick);
        std::transform(folded.begin(), folded.end(), folded.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return folded;
    }

    mutable std::shared_mutex m_mutex;
    std::unordered_map<std::string, SessionId> m_sessions;
};

// A read-mostly list of rooms and their sizes, so that listing rooms does not have
// to ask every shard. Updated only when membership changes.
class RoomDirectory {
public:
    void set_member_count(const std::string& room, std::uint32_t count) {
        const std::lock_guard lock(m_mutex);
        m_rooms[room] = count;
    }

    std::vector<protocol::RoomInfo> snapshot() const {
        const std::lock_guard lock(m_mutex);
        std::vector<protocol::RoomInfo> rooms;
        rooms.reserve(m_rooms.size());
        for (const auto& [name, count] : m_rooms) {
            rooms.push_back({name, count});
        }
        return rooms;
    }

    std::size_t size() const {
        const std::lock_guard lock(m_mutex);
        return m_rooms.size();
    }

private:
    mutable std::mutex m_mutex;
    std::map<std::string, std::uint32_t> m_rooms;
};

}  // namespace orbit::server
