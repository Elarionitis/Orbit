#include "common/timer_wheel.h"

#include <gtest/gtest.h>

#include <vector>

using orbit::TimerWheel;

TEST(TimerWheel, FiresAtTheRequestedTime) {
    TimerWheel wheel(10, 64, 0);
    int fired = 0;
    wheel.schedule(50, [&] { ++fired; });

    wheel.advance(40);
    EXPECT_EQ(fired, 0);
    wheel.advance(50);
    EXPECT_EQ(fired, 1);
    EXPECT_EQ(wheel.pending(), 0U);
}

TEST(TimerWheel, NeverFiresEarly) {
    TimerWheel wheel(10, 64, 5);
    int fired = 0;
    wheel.schedule(1, [&] { ++fired; });
    wheel.advance(9);
    EXPECT_EQ(fired, 0);
    wheel.advance(10);
    EXPECT_EQ(fired, 1);
}

TEST(TimerWheel, CancelledTimersDoNotFire) {
    TimerWheel wheel(10, 64, 0);
    int fired = 0;
    const auto id = wheel.schedule(20, [&] { ++fired; });
    EXPECT_TRUE(wheel.cancel(id));
    EXPECT_FALSE(wheel.cancel(id));
    wheel.advance(1000);
    EXPECT_EQ(fired, 0);
}

TEST(TimerWheel, HandlesDelaysBeyondOneRevolution) {
    TimerWheel wheel(10, 8, 0);  // One revolution is 80 ms.
    int fired = 0;
    wheel.schedule(250, [&] { ++fired; });

    wheel.advance(240);
    EXPECT_EQ(fired, 0);
    wheel.advance(250);
    EXPECT_EQ(fired, 1);
}

TEST(TimerWheel, ExactMultipleOfRevolutionFiresOnTime) {
    TimerWheel wheel(10, 8, 0);
    int fired = 0;
    wheel.schedule(80, [&] { ++fired; });
    wheel.advance(70);
    EXPECT_EQ(fired, 0);
    wheel.advance(80);
    EXPECT_EQ(fired, 1);
}

TEST(TimerWheel, FiresInDeadlineOrderAcrossTicks) {
    TimerWheel wheel(10, 64, 0);
    std::vector<int> order;
    wheel.schedule(30, [&] { order.push_back(3); });
    wheel.schedule(10, [&] { order.push_back(1); });
    wheel.schedule(20, [&] { order.push_back(2); });
    wheel.advance(100);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

TEST(TimerWheel, CallbackMayRescheduleItself) {
    TimerWheel wheel(10, 8, 0);
    int fired = 0;
    std::function<void()> tick = [&] {
        if (++fired < 3) {
            wheel.schedule(10, tick);
        }
    };
    wheel.schedule(10, tick);
    wheel.advance(100);
    EXPECT_EQ(fired, 3);
}

TEST(TimerWheel, CallbackMayCancelAnotherTimerInTheSameTick) {
    TimerWheel wheel(10, 64, 0);
    int fired = 0;
    TimerWheel::TimerId later = 0;
    wheel.schedule(10, [&] { wheel.cancel(later); });
    later = wheel.schedule(20, [&] { ++fired; });
    wheel.advance(100);
    EXPECT_EQ(fired, 0);
}

TEST(TimerWheel, ReportsNextDeadline) {
    TimerWheel wheel(10, 64, 0);
    EXPECT_FALSE(wheel.next_deadline_in(0).has_value());

    wheel.schedule(100, [] {});
    EXPECT_EQ(wheel.next_deadline_in(0), 100U);
    EXPECT_EQ(wheel.next_deadline_in(60), 40U);
}

TEST(TimerWheel, NextDeadlineIsConservativeForDistantTimers) {
    TimerWheel wheel(10, 8, 0);
    wheel.schedule(1000, [] {});
    const auto wait = wheel.next_deadline_in(0);
    ASSERT_TRUE(wait.has_value());
    EXPECT_LE(*wait, 1000U);
    EXPECT_GT(*wait, 0U);
}
