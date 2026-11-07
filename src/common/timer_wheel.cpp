#include "common/timer_wheel.h"

#include <algorithm>
#include <utility>

namespace orbit {

TimerWheel::TimerWheel(std::uint64_t tick_ms, std::size_t slot_count, std::uint64_t now_ms)
    : m_tick_ms(tick_ms), m_current_tick(now_ms / tick_ms), m_slots(slot_count) {}

TimerWheel::TimerId TimerWheel::schedule(std::uint64_t delay_ms, Callback callback) {
    // Round up, and always land at least one tick ahead, so a timer never fires early.
    const std::uint64_t ticks = std::max<std::uint64_t>(1, (delay_ms + m_tick_ms - 1) / m_tick_ms);
    const std::uint64_t target = m_current_tick + ticks;
    const std::size_t slot = target % m_slots.size();
    const std::uint64_t rounds = (ticks - 1) / m_slots.size();

    const TimerId id = m_next_id++;
    m_slots[slot].push_front(Entry{id, rounds, std::move(callback)});
    m_index.emplace(id, Location{slot, m_slots[slot].begin()});
    return id;
}

bool TimerWheel::cancel(TimerId id) {
    const auto found = m_index.find(id);
    if (found == m_index.end()) {
        return false;
    }
    m_slots[found->second.slot].erase(found->second.entry);
    m_index.erase(found);
    return true;
}

void TimerWheel::advance(std::uint64_t now_ms) {
    const std::uint64_t target_tick = now_ms / m_tick_ms;
    while (m_current_tick < target_tick) {
        ++m_current_tick;
        Slot& slot = m_slots[m_current_tick % m_slots.size()];

        // Detach due entries first: a callback may schedule or cancel timers.
        Slot due;
        for (auto entry = slot.begin(); entry != slot.end();) {
            const auto next = std::next(entry);
            if (entry->rounds == 0) {
                m_index.erase(entry->id);
                due.splice(due.end(), slot, entry);
            } else {
                --entry->rounds;
            }
            entry = next;
        }
        for (Entry& entry : due) {
            entry.callback();
        }
    }
}

std::optional<std::uint64_t> TimerWheel::next_deadline_in(std::uint64_t now_ms) const {
    if (m_index.empty()) {
        return std::nullopt;
    }
    for (std::size_t offset = 1; offset <= m_slots.size(); ++offset) {
        const Slot& slot = m_slots[(m_current_tick + offset) % m_slots.size()];
        const bool due_this_revolution = std::any_of(
            slot.begin(), slot.end(), [](const Entry& entry) { return entry.rounds == 0; });
        if (due_this_revolution) {
            const std::uint64_t deadline = (m_current_tick + offset) * m_tick_ms;
            return deadline > now_ms ? deadline - now_ms : 0;
        }
    }
    // Everything pending is at least one revolution away.
    return m_slots.size() * m_tick_ms;
}

}  // namespace orbit
