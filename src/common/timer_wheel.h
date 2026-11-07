#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <optional>
#include <unordered_map>
#include <vector>

namespace orbit {

// Hashed timing wheel: O(1) schedule and cancel, which matters because a server
// holds a timer per connection. Timers beyond one revolution carry a round
// count. Not thread-safe; every wheel belongs to one event loop.
class TimerWheel {
public:
    using Callback = std::function<void()>;
    using TimerId = std::uint64_t;

    TimerWheel(std::uint64_t tick_ms, std::size_t slot_count, std::uint64_t now_ms);

    TimerId schedule(std::uint64_t delay_ms, Callback callback);
    bool cancel(TimerId id);

    // Fires every timer due at or before `now_ms`.
    void advance(std::uint64_t now_ms);

    // Milliseconds until the next timer is due, or nullopt if none are pending.
    // The event loop asks on every iteration, so the answer is cached: otherwise
    // each pass would rescan the slots between now and the nearest timer.
    std::optional<std::uint64_t> next_deadline_in(std::uint64_t now_ms);

    std::size_t pending() const { return m_index.size(); }

private:
    struct Entry {
        TimerId id;
        std::uint64_t rounds;
        Callback callback;
    };
    using Slot = std::list<Entry>;
    struct Location {
        std::size_t slot;
        Slot::iterator entry;
    };

    std::uint64_t m_tick_ms;
    std::uint64_t m_current_tick;
    std::vector<Slot> m_slots;
    std::unordered_map<TimerId, Location> m_index;
    TimerId m_next_id = 1;
    std::uint64_t m_next_due_tick = 0;  // Earliest possible due tick; zero means unknown.
};

}  // namespace orbit
