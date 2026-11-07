#include "common/crc32c.h"

#include <gtest/gtest.h>

#include <array>
#include <string_view>
#include <vector>

namespace {
std::span<const std::uint8_t> bytes_of(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}
}  // namespace

// Reference vectors from RFC 3720, appendix B.4.
TEST(Crc32c, MatchesRfc3720Vectors) {
    const std::array<std::uint8_t, 32> zeros{};
    EXPECT_EQ(orbit::crc32c(zeros), 0x8A9136AAU);

    std::array<std::uint8_t, 32> ones{};
    ones.fill(0xFF);
    EXPECT_EQ(orbit::crc32c(ones), 0x62A8AB43U);

    std::array<std::uint8_t, 32> ascending{};
    for (std::size_t i = 0; i < ascending.size(); ++i) {
        ascending[i] = static_cast<std::uint8_t>(i);
    }
    EXPECT_EQ(orbit::crc32c(ascending), 0x46DD794EU);
}

TEST(Crc32c, MatchesCheckValue) {
    EXPECT_EQ(orbit::crc32c(bytes_of("123456789")), 0xE3069283U);
}

TEST(Crc32c, EmptyInputIsZero) {
    EXPECT_EQ(orbit::crc32c({}), 0U);
}

TEST(Crc32c, SupportsIncrementalComputation) {
    const auto whole = bytes_of("the quick brown fox jumps over the lazy dog");
    for (std::size_t split = 0; split <= whole.size(); ++split) {
        const auto first = orbit::crc32c(whole.first(split));
        EXPECT_EQ(orbit::crc32c(whole.subspan(split), first), orbit::crc32c(whole));
    }
}

TEST(Crc32c, DetectsSingleBitFlips) {
    std::vector<std::uint8_t> data(64, 0xA5);
    const auto original = orbit::crc32c(data);
    for (std::size_t bit = 0; bit < data.size() * 8; ++bit) {
        data[bit / 8] ^= static_cast<std::uint8_t>(1U << (bit % 8));
        EXPECT_NE(orbit::crc32c(data), original);
        data[bit / 8] ^= static_cast<std::uint8_t>(1U << (bit % 8));
    }
}
