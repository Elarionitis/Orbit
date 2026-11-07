#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

#include "common/byte_buffer.h"
#include "common/timer_wheel.h"
#include "net/event_loop.h"
#include "net/secure_channel.h"
#include "net/socket.h"

namespace orbit::net {

enum class CloseReason {
    PeerClosed,    // Orderly EOF from the peer.
    IoError,       // Reset or other socket failure.
    Local,         // Closed by this side.
    SlowConsumer,  // Output queue exceeded its bound.
    ConnectFailed,
    TlsFailure,
};

using SharedBytes = std::shared_ptr<const std::vector<std::uint8_t>>;

// A non-blocking stream socket bound to one event loop. Not thread-safe; every
// method runs on the loop thread.
//
// Output is a queue of shared immutable buffers. A broadcast costs one reference
// count per recipient rather than one copy, and the queue is flushed with a
// single vectored write once per loop iteration.
//
// close() tears the socket down at once but reports through on_close from deferred
// work, after the event batch that may still reference this object has finished.
// An owner may destroy the Connection from on_close, or at any other time on the
// loop thread: deferred work holds a liveness token rather than trusting `this`.
class Connection final : public IoHandler {
public:
    struct Handlers {
        std::function<void()> on_connected;  // Client-side connects only.
        std::function<void(ByteBuffer&)> on_data;
        std::function<void(CloseReason)> on_close;
    };

    struct Options {
        std::size_t max_queued_bytes = 0;  // Zero means unbounded.
        std::uint64_t linger_ms = 2000;    // Bound on close_after_flush().
    };

    Connection(EventLoop& loop, UniqueFd fd, Options options, Handlers handlers,
               std::unique_ptr<SecureChannel> channel = nullptr);
    ~Connection() override;

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    void start();             // For an accepted socket.
    void start_connecting();  // For a socket from connect_tcp().

    void send(SharedBytes bytes);

    void close(CloseReason reason = CloseReason::Local);

    // Stops accepting input, delivers everything queued, then closes. Bounded by
    // Options::linger_ms so that a peer that stops reading cannot hold it open.
    void close_after_flush(CloseReason reason = CloseReason::Local);

    bool is_open() const { return m_state == State::Open; }
    std::size_t queued_bytes() const { return m_queued_bytes; }

    void on_io(bool readable, bool writable, bool failed) override;

private:
    enum class State { Idle, Connecting, Open, Draining, Lingering, Closed };

    struct Chunk {
        SharedBytes bytes;
        std::size_t offset = 0;
    };

    void on_established();
    void read_available();
    void schedule_flush();
    void flush();
    void pump_channel();
    void enqueue(SharedBytes bytes);
    void update_interest();
    void begin_linger();

    // Queues `work` for the end of this loop iteration, dropping it if this object
    // has been destroyed by then.
    template <typename F>
    void defer_if_alive(F&& work) {
        m_loop.defer([alive = std::weak_ptr<void>(m_alive), work = std::forward<F>(work)] {
            if (!alive.expired()) {
                work();
            }
        });
    }

    EventLoop& m_loop;
    UniqueFd m_fd;
    Options m_options;
    Handlers m_handlers;
    std::unique_ptr<SecureChannel> m_channel;

    State m_state = State::Idle;
    CloseReason m_close_reason = CloseReason::Local;
    unsigned m_interest = kInterestNone;
    bool m_registered = false;
    bool m_flush_scheduled = false;
    TimerWheel::TimerId m_linger_timer = 0;

    ByteBuffer m_input;
    std::deque<Chunk> m_output;
    std::size_t m_queued_bytes = 0;

    std::shared_ptr<void> m_alive = std::make_shared<char>();
};

}  // namespace orbit::net
