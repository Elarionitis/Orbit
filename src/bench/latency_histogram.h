#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace orbit::bench {

// Log-linear histogram in the HdrHistogram style: each power of two is split into
// 32 linear sub-buckets, so values are recorded with about 3% precision at any
// magnitude from nanoseconds to minutes, in constant memory and constant time.
class LatencyHistogram {
public:
    void record(std::uint64_t value) {
        ++m_buckets[index_of(value)];
        ++m_count;
        if (value > m_max) {
            m_max = value;
        }
    }

    void merge(const LatencyHistogram& other) {
        for (std::size_t i = 0; i < kBuckets; ++i) {
            m_buckets[i] += other.m_buckets[i];
        }
        m_count += other.m_count;
        m_max = other.m_max > m_max ? other.m_max : m_max;
    }

    std::uint64_t count() const { return m_count; }
    std::uint64_t max() const { return m_max; }

    // The upper edge of the bucket holding the given percentile.
    std::uint64_t percentile(double percent) const {
        if (m_count == 0) {
            return 0;
        }
        const auto target =
            static_cast<std::uint64_t>(static_cast<double>(m_count) * percent / 100.0 + 0.5);
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < kBuckets; ++i) {
            seen += m_buckets[i];
            if (seen >= target && seen > 0) {
                const std::uint64_t edge = upper_edge(i);
                return edge < m_max ? edge : m_max;
            }
        }
        return m_max;
    }

private:
    static constexpr std::size_t kSubBits = 5;
    static constexpr std::size_t kSubBuckets = std::size_t{1} << kSubBits;
    static constexpr std::size_t kBuckets = (64 - kSubBits) * kSubBuckets;

    static std::size_t index_of(std::uint64_t value) {
        if (value < kSubBuckets) {
            return static_cast<std::size_t>(value);
        }
        const auto exponent = static_cast<std::size_t>(std::bit_width(value)) - 1;
        const std::size_t shift = exponent - kSubBits;
        const auto sub = static_cast<std::size_t>(value >> shift) - kSubBuckets;
        return (shift + 1) * kSubBuckets + sub;
    }

    static std::uint64_t upper_edge(std::size_t index) {
        if (index < kSubBuckets) {
            return index;
        }
        const std::size_t shift = index / kSubBuckets - 1;
        const std::size_t sub = index % kSubBuckets;
        return ((static_cast<std::uint64_t>(kSubBuckets + sub) + 1) << shift) - 1;
    }

    std::array<std::uint64_t, kBuckets> m_buckets{};
    std::uint64_t m_count = 0;
    std::uint64_t m_max = 0;
};

}  // namespace orbit::bench
