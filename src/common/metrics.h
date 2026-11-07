#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::metrics {

// Counter striped across cache lines. Event-loop threads update counters on the
// hot path; a single shared atomic would bounce one cache line between cores on
// every message. The stripes are summed only when somebody scrapes.
class Counter {
public:
    void add(std::uint64_t amount = 1);
    std::uint64_t value() const;

private:
    static constexpr std::size_t kStripes = 16;
    struct alignas(64) Stripe {
        std::atomic<std::uint64_t> value{0};
    };
    std::array<Stripe, kStripes> m_stripes;
};

class Gauge {
public:
    void add(std::int64_t amount = 1) { m_value.fetch_add(amount, std::memory_order_relaxed); }
    void sub(std::int64_t amount = 1) { m_value.fetch_sub(amount, std::memory_order_relaxed); }
    void set(std::int64_t value) { m_value.store(value, std::memory_order_relaxed); }
    std::int64_t value() const { return m_value.load(std::memory_order_relaxed); }

private:
    std::atomic<std::int64_t> m_value{0};
};

// Fixed-bucket histogram in the Prometheus cumulative style.
class Histogram {
public:
    explicit Histogram(std::vector<double> upper_bounds);

    void observe(double value);

    const std::vector<double>& bounds() const { return m_bounds; }
    std::uint64_t bucket(std::size_t index) const;  // Non-cumulative; last bucket is +Inf.
    std::uint64_t count() const { return m_count.load(std::memory_order_relaxed); }
    double sum() const;

private:
    std::vector<double> m_bounds;
    std::unique_ptr<std::atomic<std::uint64_t>[]> m_buckets;
    std::atomic<std::uint64_t> m_count{0};
    std::atomic<std::uint64_t> m_sum_micros{0};
};

// Owns all metrics and renders them in the Prometheus text exposition format.
// Registration takes a lock and happens at startup; afterwards callers hold a
// stable reference and never touch the registry on the hot path.
class Registry {
public:
    Counter& counter(std::string_view name, std::string_view help, std::string_view labels = {});
    Gauge& gauge(std::string_view name, std::string_view help, std::string_view labels = {});
    Histogram& histogram(std::string_view name, std::string_view help,
                         std::vector<double> upper_bounds);

    std::string render() const;

private:
    enum class Kind { Counter, Gauge, Histogram };
    struct Series {
        std::string labels;
        Counter* counter = nullptr;
        Gauge* gauge = nullptr;
        Histogram* histogram = nullptr;
    };
    struct Family {
        std::string help;
        Kind kind;
        std::vector<Series> series;
    };

    Family& family(std::string_view name, std::string_view help, Kind kind);

    mutable std::mutex m_mutex;
    std::map<std::string, Family, std::less<>> m_families;
    std::deque<Counter> m_counters;
    std::deque<Gauge> m_gauges;
    std::deque<Histogram> m_histograms;
};

}  // namespace orbit::metrics
