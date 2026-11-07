#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace orbit {

// Contiguous FIFO byte buffer with separate read and write cursors.
//
// Readers see one flat span, which lets the frame decoder parse in place
// without copying a message out of a ring. Consuming only moves a cursor; the
// remaining bytes are compacted lazily when the writer runs out of tail room.
class ByteBuffer {
public:
    ByteBuffer() = default;
    explicit ByteBuffer(std::size_t capacity) : m_data(capacity) {}

    std::size_t size() const { return m_write - m_read; }
    bool empty() const { return m_read == m_write; }
    std::size_t capacity() const { return m_data.size(); }

    std::span<const std::uint8_t> readable() const {
        return {m_data.data() + m_read, m_write - m_read};
    }

    void consume(std::size_t count) {
        m_read += std::min(count, size());
        if (m_read == m_write) {
            m_read = 0;
            m_write = 0;
        }
    }

    // Returns a writable region of at least `min_size` bytes. Follow with commit().
    std::span<std::uint8_t> prepare(std::size_t min_size) {
        if (m_data.size() - m_write < min_size) {
            make_room(min_size);
        }
        return {m_data.data() + m_write, m_data.size() - m_write};
    }

    void commit(std::size_t count) { m_write = std::min(m_write + count, m_data.size()); }

    void append(std::span<const std::uint8_t> bytes) {
        if (bytes.empty()) {
            return;
        }
        auto target = prepare(bytes.size());
        std::memcpy(target.data(), bytes.data(), bytes.size());
        commit(bytes.size());
    }

    void clear() {
        m_read = 0;
        m_write = 0;
    }

    // Returns memory to the allocator once a burst has drained, so that an idle
    // connection does not pin the high-water mark of its busiest moment.
    void shrink_if_idle(std::size_t keep) {
        if (empty() && m_data.size() > keep) {
            std::vector<std::uint8_t>(keep).swap(m_data);
        }
    }

private:
    void make_room(std::size_t min_size) {
        const std::size_t live = size();
        if (m_data.size() - live >= min_size) {
            if (live > 0) {
                std::memmove(m_data.data(), m_data.data() + m_read, live);
            }
        } else {
            std::vector<std::uint8_t> grown(std::max(m_data.size() * 2, live + min_size));
            if (live > 0) {
                std::memcpy(grown.data(), m_data.data() + m_read, live);
            }
            m_data.swap(grown);
        }
        m_read = 0;
        m_write = live;
    }

    std::vector<std::uint8_t> m_data;
    std::size_t m_read = 0;
    std::size_t m_write = 0;
};

}  // namespace orbit
