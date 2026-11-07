#include "common/crc32c.h"

#include <array>

namespace orbit {
namespace {

constexpr std::uint32_t kPolynomial = 0x82F63B78U;  // Reflected Castagnoli polynomial.
constexpr std::size_t kSlices = 8;

using Table = std::array<std::array<std::uint32_t, 256>, kSlices>;

constexpr Table make_table() {
    Table table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t crc = i;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0 ? (crc >> 1) ^ kPolynomial : crc >> 1;
        }
        table[0][i] = crc;
    }
    for (std::uint32_t i = 0; i < 256; ++i) {
        for (std::size_t slice = 1; slice < kSlices; ++slice) {
            const std::uint32_t previous = table[slice - 1][i];
            table[slice][i] = (previous >> 8) ^ table[0][previous & 0xFFU];
        }
    }
    return table;
}

constexpr Table kTable = make_table();

}  // namespace

std::uint32_t crc32c(std::span<const std::uint8_t> bytes, std::uint32_t seed) {
    std::uint32_t crc = ~seed;
    const std::uint8_t* cursor = bytes.data();
    std::size_t remaining = bytes.size();

    // Slicing-by-8: fold eight input bytes per step using eight lookup tables.
    while (remaining >= kSlices) {
        const std::uint32_t low = crc ^ (static_cast<std::uint32_t>(cursor[0]) |
                                         static_cast<std::uint32_t>(cursor[1]) << 8 |
                                         static_cast<std::uint32_t>(cursor[2]) << 16 |
                                         static_cast<std::uint32_t>(cursor[3]) << 24);
        crc = kTable[7][low & 0xFFU] ^ kTable[6][(low >> 8) & 0xFFU] ^
              kTable[5][(low >> 16) & 0xFFU] ^ kTable[4][low >> 24] ^ kTable[3][cursor[4]] ^
              kTable[2][cursor[5]] ^ kTable[1][cursor[6]] ^ kTable[0][cursor[7]];
        cursor += kSlices;
        remaining -= kSlices;
    }
    while (remaining-- > 0) {
        crc = kTable[0][(crc ^ *cursor++) & 0xFFU] ^ (crc >> 8);
    }
    return ~crc;
}

}  // namespace orbit
