#include "common/byte_buffer.h"

#include <gtest/gtest.h>

#include <numeric>
#include <vector>

using orbit::ByteBuffer;

namespace {
std::vector<std::uint8_t> sequence(std::size_t count, std::uint8_t first = 0) {
    std::vector<std::uint8_t> bytes(count);
    std::iota(bytes.begin(), bytes.end(), first);
    return bytes;
}
}  // namespace

TEST(ByteBuffer, StartsEmpty) {
    ByteBuffer buffer;
    EXPECT_TRUE(buffer.empty());
    EXPECT_EQ(buffer.size(), 0U);
    EXPECT_TRUE(buffer.readable().empty());
}

TEST(ByteBuffer, AppendThenReadBack) {
    ByteBuffer buffer;
    const auto bytes = sequence(100);
    buffer.append(bytes);
    ASSERT_EQ(buffer.size(), 100U);
    EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), buffer.readable().begin()));
}

TEST(ByteBuffer, ConsumingEverythingResetsCursors) {
    ByteBuffer buffer(16);
    buffer.append(sequence(16));
    buffer.consume(16);
    EXPECT_TRUE(buffer.empty());
    EXPECT_GE(buffer.prepare(16).size(), 16U);
    EXPECT_EQ(buffer.capacity(), 16U);  // Reused rather than reallocated.
}

TEST(ByteBuffer, ConsumeIsClampedToSize) {
    ByteBuffer buffer;
    buffer.append(sequence(4));
    buffer.consume(1000);
    EXPECT_TRUE(buffer.empty());
}

TEST(ByteBuffer, CompactsInsteadOfGrowingWhenThereIsSlack) {
    ByteBuffer buffer(32);
    buffer.append(sequence(32));
    buffer.consume(24);
    buffer.append(sequence(20, 100));

    EXPECT_EQ(buffer.capacity(), 32U);
    ASSERT_EQ(buffer.size(), 28U);
    EXPECT_EQ(buffer.readable()[0], 24);
    EXPECT_EQ(buffer.readable()[8], 100);
}

TEST(ByteBuffer, GrowsPreservingLiveBytes) {
    ByteBuffer buffer(8);
    buffer.append(sequence(8));
    buffer.consume(2);
    buffer.append(sequence(100, 50));

    ASSERT_EQ(buffer.size(), 106U);
    EXPECT_EQ(buffer.readable()[0], 2);
    EXPECT_EQ(buffer.readable()[6], 50);
}

TEST(ByteBuffer, PrepareAndCommitExposeWrittenBytes) {
    ByteBuffer buffer;
    auto region = buffer.prepare(4);
    ASSERT_GE(region.size(), 4U);
    region[0] = 7;
    region[1] = 9;
    buffer.commit(2);
    ASSERT_EQ(buffer.size(), 2U);
    EXPECT_EQ(buffer.readable()[1], 9);
}

TEST(ByteBuffer, ShrinkReleasesMemoryOnlyWhenIdle) {
    ByteBuffer buffer;
    buffer.append(sequence(200));
    const auto capacity = buffer.capacity();

    buffer.shrink_if_idle(16);
    EXPECT_EQ(buffer.capacity(), capacity);  // Live bytes must survive.

    buffer.consume(200);
    buffer.shrink_if_idle(16);
    EXPECT_EQ(buffer.capacity(), 16U);
}
