#include "bench/latency_histogram.h"

#include <gtest/gtest.h>

#include <memory>

using orbit::bench::LatencyHistogram;

namespace {
auto make_histogram() {
    return std::make_unique<LatencyHistogram>();
}
}  // namespace

TEST(LatencyHistogram, EmptyReportsZero) {
    const auto histogram = make_histogram();
    EXPECT_EQ(histogram->percentile(99), 0U);
    EXPECT_EQ(histogram->count(), 0U);
}

TEST(LatencyHistogram, IsExactForSmallValues) {
    auto histogram = make_histogram();
    for (std::uint64_t value = 1; value <= 10; ++value) {
        histogram->record(value);
    }
    EXPECT_EQ(histogram->percentile(50), 5U);
    EXPECT_EQ(histogram->percentile(100), 10U);
}

TEST(LatencyHistogram, StaysWithinThreePercentAcrossMagnitudes) {
    for (std::uint64_t value = 100; value < 100'000'000'000ULL; value = value * 3 + 7) {
        auto histogram = make_histogram();
        histogram->record(value);
        histogram->record(value * 2);  // Keeps max from masking the bucket edge.
        const auto reported = histogram->percentile(50);
        EXPECT_GE(reported, value);
        EXPECT_LE(static_cast<double>(reported), static_cast<double>(value) * 1.04);
    }
}

TEST(LatencyHistogram, PercentilesAreOrderedAndCappedByMax) {
    auto histogram = make_histogram();
    for (std::uint64_t i = 1; i <= 100'000; ++i) {
        histogram->record(i * 1000);
    }
    EXPECT_LE(histogram->percentile(50), histogram->percentile(90));
    EXPECT_LE(histogram->percentile(90), histogram->percentile(99));
    EXPECT_LE(histogram->percentile(99), histogram->percentile(99.9));
    EXPECT_LE(histogram->percentile(100), histogram->max());
    EXPECT_NEAR(static_cast<double>(histogram->percentile(50)), 50'000'000.0, 2'000'000.0);
}

TEST(LatencyHistogram, MergesCounts) {
    auto first = make_histogram();
    auto second = make_histogram();
    first->record(10);
    second->record(1000);
    first->merge(*second);
    EXPECT_EQ(first->count(), 2U);
    EXPECT_EQ(first->max(), 1000U);
}
