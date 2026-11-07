#include "common/token_bucket.h"

#include <gtest/gtest.h>

using orbit::TokenBucket;

TEST(TokenBucket, AllowsTheInitialBurst) {
    TokenBucket bucket(10.0, 5.0);
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(bucket.try_acquire(0));
    }
    EXPECT_FALSE(bucket.try_acquire(0));
}

TEST(TokenBucket, RefillsAtTheConfiguredRate) {
    TokenBucket bucket(10.0, 1.0);
    EXPECT_TRUE(bucket.try_acquire(1000));
    EXPECT_FALSE(bucket.try_acquire(1000));
    EXPECT_FALSE(bucket.try_acquire(1050));  // Half a token.
    EXPECT_TRUE(bucket.try_acquire(1100));   // One full token.
}

TEST(TokenBucket, NeverExceedsBurstAfterLongIdle) {
    TokenBucket bucket(100.0, 3.0);
    int granted = 0;
    while (bucket.try_acquire(1'000'000)) {
        ++granted;
    }
    EXPECT_EQ(granted, 3);
}

TEST(TokenBucket, ZeroRateDisablesLimiting) {
    TokenBucket bucket(0.0, 0.0);
    for (int i = 0; i < 10'000; ++i) {
        EXPECT_TRUE(bucket.try_acquire(0));
    }
}

TEST(TokenBucket, ToleratesTimeGoingBackwards) {
    TokenBucket bucket(1.0, 1.0);
    EXPECT_TRUE(bucket.try_acquire(5000));
    EXPECT_FALSE(bucket.try_acquire(4000));
}
