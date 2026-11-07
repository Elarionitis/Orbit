#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::protocol {

// Appends big-endian primitives to a byte vector.
class Writer {
public:
    explicit Writer(std::vector<std::uint8_t>& out) : m_out(out) {}

    void u8(std::uint8_t value) { m_out.push_back(value); }
    void u16(std::uint16_t value) { put(value); }
    void u32(std::uint32_t value) { put(value); }
    void u64(std::uint64_t value) { put(value); }

    // Short strings (nicknames, room names) carry a 16-bit length.
    void str(std::string_view value) {
        const auto length = static_cast<std::uint16_t>(
            std::min<std::size_t>(value.size(), std::numeric_limits<std::uint16_t>::max()));
        u16(length);
        bytes(value.substr(0, length));
    }

    // Message bodies carry a 32-bit length.
    void blob(std::string_view value) {
        u32(static_cast<std::uint32_t>(value.size()));
        bytes(value);
    }

private:
    template <typename T>
    void put(T value) {
        for (int shift = (static_cast<int>(sizeof(T)) - 1) * 8; shift >= 0; shift -= 8) {
            m_out.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }
    void bytes(std::string_view value) {
        const auto* data = reinterpret_cast<const std::uint8_t*>(value.data());
        m_out.insert(m_out.end(), data, data + value.size());
    }

    std::vector<std::uint8_t>& m_out;
};

// Reads big-endian primitives from a span. Every read is bounds-checked; after
// the first short read the reader latches into a failed state and returns
// zero values, so decoders can read all their fields and test ok() once.
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> input) : m_input(input) {}

    std::uint8_t u8() { return get<std::uint8_t>(); }
    std::uint16_t u16() { return get<std::uint16_t>(); }
    std::uint32_t u32() { return get<std::uint32_t>(); }
    std::uint64_t u64() { return get<std::uint64_t>(); }

    std::string str() { return bytes(u16()); }
    std::string blob() { return bytes(u32()); }

    bool ok() const { return !m_failed; }
    bool at_end() const { return m_position == m_input.size(); }
    std::size_t remaining() const { return m_input.size() - m_position; }

private:
    template <typename T>
    T get() {
        if (remaining() < sizeof(T) || m_failed) {
            m_failed = true;
            return T{};
        }
        T value{};
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            value = static_cast<T>(static_cast<std::uint64_t>(value) << 8 |
                                   m_input[m_position + i]);
        }
        m_position += sizeof(T);
        return value;
    }

    std::string bytes(std::size_t count) {
        if (remaining() < count || m_failed) {
            m_failed = true;
            return {};
        }
        std::string value(reinterpret_cast<const char*>(m_input.data() + m_position), count);
        m_position += count;
        return value;
    }

    std::span<const std::uint8_t> m_input;
    std::size_t m_position = 0;
    bool m_failed = false;
};

}  // namespace orbit::protocol
