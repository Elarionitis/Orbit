#pragma once

#include <chrono>
#include <cstdint>

namespace orbit {

// Monotonic time, for measuring intervals.
inline std::uint64_t monotonic_ms() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

inline std::uint64_t monotonic_ns() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

// Wall-clock time, for timestamps shown to people.
inline std::uint64_t wall_clock_ms() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

}  // namespace orbit
