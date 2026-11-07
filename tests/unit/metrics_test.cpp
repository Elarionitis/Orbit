#include "common/metrics.h"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

using namespace orbit::metrics;

TEST(Counter, SumsAcrossThreads) {
    Counter counter;
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 10'000; ++i) {
                counter.add();
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(counter.value(), 80'000U);
}

TEST(Histogram, PlacesObservationsInBuckets) {
    Histogram histogram({0.1, 1.0, 10.0});
    histogram.observe(0.05);
    histogram.observe(0.5);
    histogram.observe(1.0);  // Upper bounds are inclusive.
    histogram.observe(100.0);

    EXPECT_EQ(histogram.bucket(0), 1U);
    EXPECT_EQ(histogram.bucket(1), 2U);
    EXPECT_EQ(histogram.bucket(2), 0U);
    EXPECT_EQ(histogram.bucket(3), 1U);
    EXPECT_EQ(histogram.count(), 4U);
    EXPECT_NEAR(histogram.sum(), 101.55, 1e-6);
}

TEST(Registry, RendersPrometheusText) {
    Registry registry;
    registry.counter("orbit_things_total", "Things seen.").add(3);
    registry.gauge("orbit_level", "Current level.").set(-2);

    const auto text = registry.render();
    EXPECT_NE(text.find("# HELP orbit_things_total Things seen.\n"), std::string::npos);
    EXPECT_NE(text.find("# TYPE orbit_things_total counter\n"), std::string::npos);
    EXPECT_NE(text.find("orbit_things_total 3\n"), std::string::npos);
    EXPECT_NE(text.find("# TYPE orbit_level gauge\n"), std::string::npos);
    EXPECT_NE(text.find("orbit_level -2\n"), std::string::npos);
}

TEST(Registry, GroupsLabelledSeriesUnderOneHeader) {
    Registry registry;
    registry.counter("orbit_closed_total", "Closed.", "reason=\"eof\"").add(1);
    registry.counter("orbit_closed_total", "Closed.", "reason=\"slow\"").add(2);

    const auto text = registry.render();
    EXPECT_EQ(text.find("# TYPE orbit_closed_total"), text.rfind("# TYPE orbit_closed_total"));
    EXPECT_NE(text.find("orbit_closed_total{reason=\"eof\"} 1\n"), std::string::npos);
    EXPECT_NE(text.find("orbit_closed_total{reason=\"slow\"} 2\n"), std::string::npos);
}

TEST(Registry, RendersCumulativeHistogramBuckets) {
    Registry registry;
    auto& histogram = registry.histogram("orbit_latency_seconds", "Latency.", {0.5, 1.0});
    histogram.observe(0.1);
    histogram.observe(0.7);
    histogram.observe(5.0);

    const auto text = registry.render();
    EXPECT_NE(text.find("orbit_latency_seconds_bucket{le=\"0.5\"} 1\n"), std::string::npos);
    EXPECT_NE(text.find("orbit_latency_seconds_bucket{le=\"1\"} 2\n"), std::string::npos);
    EXPECT_NE(text.find("orbit_latency_seconds_bucket{le=\"+Inf\"} 3\n"), std::string::npos);
    EXPECT_NE(text.find("orbit_latency_seconds_count 3\n"), std::string::npos);
}
