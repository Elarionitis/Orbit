#include "net/connection.h"

#include <sys/socket.h>
#include <sys/uio.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <utility>

namespace orbit::net {
namespace {

constexpr std::size_t kReadChunk = 64 * 1024;
constexpr std::size_t kInputKeep = 4 * 1024;
constexpr std::size_t kMaxIovecs = 64;

// One scratch buffer per thread rather than per connection: at 10,000 idle
// connections, per-connection receive buffers would pin hundreds of megabytes.
std::array<std::uint8_t, kReadChunk>& read_scratch() {
    thread_local std::array<std::uint8_t, kReadChunk> scratch;
    return scratch;
}

}  // namespace

Connection::Connection(EventLoop& loop, UniqueFd fd, Options options, Handlers handlers,
                       std::unique_ptr<SecureChannel> channel)
    : m_loop(loop),
      m_fd(std::move(fd)),
      m_options(options),
      m_handlers(std::move(handlers)),
      m_channel(std::move(channel)) {}

Connection::~Connection() {
    if (m_linger_timer != 0) {
        m_loop.cancel_timer(m_linger_timer);
    }
    if (m_registered) {
        m_loop.remove(m_fd.get());
    }
}

void Connection::start() {
    set_nonblocking(m_fd.get());
    configure_stream_socket(m_fd.get());
    m_interest = kInterestRead;
    m_loop.add(m_fd.get(), m_interest, this);
    m_registered = true;
    on_established();
}

void Connection::start_connecting() {
    m_state = State::Connecting;
    m_interest = kInterestWrite;
    m_loop.add(m_fd.get(), m_interest, this);
    m_registered = true;
}

void Connection::on_established() {
    m_state = State::Open;
    if (m_channel) {
        if (!m_channel->start()) {
            close(CloseReason::TlsFailure);
            return;
        }
        pump_channel();
    }
}

void Connection::on_io(bool readable, bool writable, bool failed) {
    if (m_state == State::Closed) {
        return;  // A stale event from earlier in this batch.
    }
    if (m_state == State::Connecting) {
        const int error = connect_result(m_fd.get());
        if (error != 0) {
            close(CloseReason::ConnectFailed);
            return;
        }
        m_interest = kInterestRead;
        update_interest();
        on_established();
        if (m_state == State::Open && m_handlers.on_connected) {
            m_handlers.on_connected();
        }
        return;
    }
    if (writable) {
        flush();
    }
    if ((readable || failed) && m_state != State::Closed) {
        read_available();
    }
}

void Connection::read_available() {
    auto& scratch = read_scratch();
    // One read per readiness event: level-triggered polling calls back for the
    // rest, which stops a firehose client from starving its neighbours.
    const ssize_t received = ::recv(m_fd.get(), scratch.data(), scratch.size(), 0);
    if (received == 0) {
        close(m_state == State::Open ? CloseReason::PeerClosed : m_close_reason);
        return;
    }
    if (received < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            close(m_state == State::Open ? CloseReason::IoError : m_close_reason);
        }
        return;
    }
    if (m_state != State::Open) {
        return;  // Draining: the input no longer matters, only the EOF does.
    }

    const std::span<const std::uint8_t> bytes(scratch.data(), static_cast<std::size_t>(received));
    if (m_channel) {
        if (!m_channel->receive(bytes, m_input)) {
            pump_channel();  // Let any TLS alert go out.
            close_after_flush(CloseReason::TlsFailure);
            return;
        }
        pump_channel();
    } else {
        m_input.append(bytes);
    }

    if (!m_input.empty()) {
        m_handlers.on_data(m_input);
    }
    m_input.shrink_if_idle(kInputKeep);
}

void Connection::send(SharedBytes bytes) {
    if (m_state != State::Open || !bytes || bytes->empty()) {
        return;
    }
    if (m_channel) {
        if (!m_channel->send(*bytes)) {
            close(CloseReason::TlsFailure);
            return;
        }
        pump_channel();
    } else {
        enqueue(std::move(bytes));
    }
}

void Connection::pump_channel() {
    auto ciphertext = std::make_shared<std::vector<std::uint8_t>>();
    m_channel->take_output(*ciphertext);
    if (!ciphertext->empty()) {
        enqueue(std::move(ciphertext));
    }
}

void Connection::enqueue(SharedBytes bytes) {
    m_queued_bytes += bytes->size();
    m_output.push_back(Chunk{std::move(bytes), 0});
    if (m_options.max_queued_bytes != 0 && m_queued_bytes > m_options.max_queued_bytes) {
        // Isolate the slow reader. Buffering without bound would let one stalled
        // client exhaust server memory; blocking would stall everyone else. It
        // can reconnect and catch up from its last sequence number.
        close(CloseReason::SlowConsumer);
        return;
    }
    schedule_flush();
}

void Connection::schedule_flush() {
    if (!m_flush_scheduled) {
        m_flush_scheduled = true;
        defer_if_alive([this] {
            m_flush_scheduled = false;
            flush();
        });
    }
}

void Connection::flush() {
    if (m_state != State::Open && m_state != State::Draining) {
        return;
    }
    while (!m_output.empty()) {
        std::array<iovec, kMaxIovecs> vectors{};
        std::size_t count = 0;
        for (const Chunk& chunk : m_output) {
            if (count == kMaxIovecs) {
                break;
            }
            // iovec is a C API: the const_cast does not lead to a write.
            vectors[count].iov_base = const_cast<std::uint8_t*>(chunk.bytes->data() + chunk.offset);
            vectors[count].iov_len = chunk.bytes->size() - chunk.offset;
            ++count;
        }

        msghdr message{};
        message.msg_iov = vectors.data();
        message.msg_iovlen = static_cast<decltype(message.msg_iovlen)>(count);
        const ssize_t sent = ::sendmsg(m_fd.get(), &message, send_flags());
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            close(m_state == State::Open ? CloseReason::IoError : m_close_reason);
            return;
        }

        auto remaining = static_cast<std::size_t>(sent);
        m_queued_bytes -= remaining;
        while (remaining > 0) {
            Chunk& front = m_output.front();
            const std::size_t available = front.bytes->size() - front.offset;
            if (remaining < available) {
                front.offset += remaining;  // Partial write: resume here next time.
                break;
            }
            remaining -= available;
            m_output.pop_front();
        }
    }

    const unsigned wanted =
        (m_interest & kInterestRead) | (m_output.empty() ? kInterestNone : kInterestWrite);
    if (wanted != m_interest) {
        m_interest = wanted;
        update_interest();
    }
    if (m_output.empty() && m_state == State::Draining) {
        begin_linger();
    }
}

void Connection::update_interest() {
    m_loop.modify(m_fd.get(), m_interest, this);
}

void Connection::close_after_flush(CloseReason reason) {
    if (m_state != State::Open) {
        if (m_state == State::Idle || m_state == State::Connecting) {
            close(reason);
        }
        return;
    }
    m_state = State::Draining;
    m_close_reason = reason;
    m_linger_timer = m_loop.run_after(m_options.linger_ms, [this] {
        m_linger_timer = 0;
        close(m_close_reason);
    });
    schedule_flush();
}

void Connection::begin_linger() {
    // Half-close, then wait for the peer's EOF. Closing outright while its bytes
    // sit unread here would send a RST, and that can destroy the final error
    // frame before the peer gets to read it.
    m_state = State::Lingering;
    ::shutdown(m_fd.get(), SHUT_WR);
}

void Connection::close(CloseReason reason) {
    if (m_state == State::Closed) {
        return;
    }
    const bool graceful = m_state == State::Draining || m_state == State::Lingering;
    m_state = State::Closed;
    if (m_linger_timer != 0) {
        m_loop.cancel_timer(m_linger_timer);
        m_linger_timer = 0;
    }
    if (m_registered) {
        m_loop.remove(m_fd.get());
        m_registered = false;
    }
    m_fd.reset();
    m_output.clear();
    m_queued_bytes = 0;

    const CloseReason reported = graceful ? m_close_reason : reason;
    defer_if_alive([this, reported] {
        // Moved out first: the handler is allowed to destroy this object, and a
        // std::function must not be destroyed while it is running.
        const auto on_close = std::move(m_handlers.on_close);
        if (on_close) {
            on_close(reported);
        }
    });
}

}  // namespace orbit::net
