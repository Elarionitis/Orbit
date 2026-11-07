#include "common/metrics.h"

#include <algorithm>
#include <format>
#include <utility>

namespace orbit::metrics {
namespace {

// Spreads threads across stripes round-robin the first time each one counts.
std::size_t stripe_for_this_thread(std::size_t stripes) {
    static std::atomic<std::size_t> next{0};
    thread_local const std::size_t assigned = next.fetch_add(1, std::memory_order_relaxed);
    return assigned % stripes;
}

constexpr double kMicrosPerUnit = 1'000'000.0;

}  // namespace

void Counter::add(std::uint64_t amount) {
    m_stripes[stripe_for_this_thread(kStripes)].value.fetch_add(amount, std::memory_order_relaxed);
}

std::uint64_t Counter::value() const {
    std::uint64_t total = 0;
    for (const Stripe& stripe : m_stripes) {
        total += stripe.value.load(std::memory_order_relaxed);
    }
    return total;
}

Histogram::Histogram(std::vector<double> upper_bounds)
    : m_bounds(std::move(upper_bounds)),
      m_buckets(std::make_unique<std::atomic<std::uint64_t>[]>(m_bounds.size() + 1)) {
    std::sort(m_bounds.begin(), m_bounds.end());
}

void Histogram::observe(double value) {
    const auto bound = std::lower_bound(m_bounds.begin(), m_bounds.end(), value);
    const auto index = static_cast<std::size_t>(bound - m_bounds.begin());
    m_buckets[index].fetch_add(1, std::memory_order_relaxed);
    m_count.fetch_add(1, std::memory_order_relaxed);
    m_sum_micros.fetch_add(static_cast<std::uint64_t>(std::max(value, 0.0) * kMicrosPerUnit),
                           std::memory_order_relaxed);
}

std::uint64_t Histogram::bucket(std::size_t index) const {
    return m_buckets[index].load(std::memory_order_relaxed);
}

double Histogram::sum() const {
    return static_cast<double>(m_sum_micros.load(std::memory_order_relaxed)) / kMicrosPerUnit;
}

Registry::Family& Registry::family(std::string_view name, std::string_view help, Kind kind) {
    const auto found = m_families.find(name);
    if (found != m_families.end()) {
        return found->second;
    }
    return m_families.emplace(std::string(name), Family{std::string(help), kind, {}}).first->second;
}

Counter& Registry::counter(std::string_view name, std::string_view help, std::string_view labels) {
    const std::lock_guard lock(m_mutex);
    Counter& counter = m_counters.emplace_back();
    family(name, help, Kind::Counter)
        .series.push_back(Series{std::string(labels), &counter, nullptr, nullptr});
    return counter;
}

Gauge& Registry::gauge(std::string_view name, std::string_view help, std::string_view labels) {
    const std::lock_guard lock(m_mutex);
    Gauge& gauge = m_gauges.emplace_back();
    family(name, help, Kind::Gauge)
        .series.push_back(Series{std::string(labels), nullptr, &gauge, nullptr});
    return gauge;
}

Histogram& Registry::histogram(std::string_view name, std::string_view help,
                               std::vector<double> upper_bounds) {
    const std::lock_guard lock(m_mutex);
    Histogram& histogram = m_histograms.emplace_back(std::move(upper_bounds));
    family(name, help, Kind::Histogram)
        .series.push_back(Series{std::string(), nullptr, nullptr, &histogram});
    return histogram;
}

std::string Registry::render() const {
    const std::lock_guard lock(m_mutex);
    std::string out;
    for (const auto& [name, family] : m_families) {
        const char* type = family.kind == Kind::Counter ? "counter"
                           : family.kind == Kind::Gauge ? "gauge"
                                                        : "histogram";
        out += std::format("# HELP {} {}\n# TYPE {} {}\n", name, family.help, name, type);
        for (const Series& series : family.series) {
            const std::string labels =
                series.labels.empty() ? std::string() : std::format("{{{}}}", series.labels);
            if (series.counter != nullptr) {
                out += std::format("{}{} {}\n", name, labels, series.counter->value());
            } else if (series.gauge != nullptr) {
                out += std::format("{}{} {}\n", name, labels, series.gauge->value());
            } else {
                const Histogram& histogram = *series.histogram;
                std::uint64_t cumulative = 0;
                for (std::size_t i = 0; i < histogram.bounds().size(); ++i) {
                    cumulative += histogram.bucket(i);
                    out += std::format("{}_bucket{{le=\"{}\"}} {}\n", name, histogram.bounds()[i],
                                       cumulative);
                }
                cumulative += histogram.bucket(histogram.bounds().size());
                out += std::format("{}_bucket{{le=\"+Inf\"}} {}\n", name, cumulative);
                out += std::format("{}_sum {}\n{}_count {}\n", name, histogram.sum(), name,
                                   histogram.count());
            }
        }
    }
    return out;
}

}  // namespace orbit::metrics
