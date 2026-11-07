#pragma once

#include <algorithm>
#include <cstdint>

namespace orbit {

// Token-bucket rate limiter. Tokens accrue lazily from elapsed time, so an idle
// session costs nothing and no timer is needed. Not thread-safe: each bucket is
// owned by a single session on a single event loop.
class TokenBucket {
public:
    // A rate of zero disables limiting.
    TokenBucket(double rate_per_second, double burst)
        : m_rate(rate_per_second), m_burst(burst), m_tokens(burst) {}

    bool try_acquire(std::uint64_t now_ms, double tokens = 1.0) {
        if (m_rate <= 0.0) {
            return true;
        }
        if (now_ms > m_last_ms) {
            const auto elapsed_ms = static_cast<double>(now_ms - m_last_ms);
            m_tokens = std::min(m_burst, m_tokens + elapsed_ms * m_rate / 1000.0);
            m_last_ms = now_ms;
        }
        if (m_tokens < tokens) {
            return false;
        }
        m_tokens -= tokens;
        return true;
    }

private:
    double m_rate;
    double m_burst;
    double m_tokens;
    std::uint64_t m_last_ms = 0;
};

}  // namespace orbit
