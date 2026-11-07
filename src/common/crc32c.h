#pragma once

#include <cstdint>
#include <span>

namespace orbit {

// CRC-32C (Castagnoli). Chosen over CRC-32 for its stronger error detection on
// the record sizes the write-ahead log produces.
std::uint32_t crc32c(std::span<const std::uint8_t> bytes, std::uint32_t seed = 0);

}  // namespace orbit
