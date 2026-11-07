#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "common/timer_wheel.h"
#include "net/poller.h"

namespace orbit::net {

class IoHandler {
public:
    virtual ~IoHandler() = default;
    virtual void on_io(bool readable, bool writable, bool failed) = 0;
};

// Single-threaded reactor: one thread owns everything registered here, so that
// state needs no locks. post() is the only thread-safe entry point, and it is
// how threads talk to each other: work moves to the data, not the reverse.
//
// One iteration runs in four phases:
//   1. dispatch socket readiness
//   2. run tasks posted by other threads
//   3. fire timers
//   4. run deferred work
// Deferring writes to phase 4 is what batches them. However many frames land on
// a connection during phases 1-3, they leave in one vectored syscall.
class EventLoop {
public:
    using Task = std::function<void()>;

    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void run();

    // Thread-safe. Returns immediately; run() exits after the current iteration.
    void stop();

    // Thread-safe. Tasks from one thread run in the order they were posted, the
    // property the room ordering guarantee is built on.
    void post(Task task);

    // Tasks posted but not yet run. Used for load shedding.
    std::size_t queue_depth() const { return m_queue_depth.load(std::memory_order_relaxed); }

    bool in_loop_thread() const { return std::this_thread::get_id() == m_thread_id.load(); }

    // Everything below must be called on the loop thread.
    void defer(Task task) { m_deferred.push_back(std::move(task)); }

    void add(int fd, unsigned interest, IoHandler* handler) { m_poller.add(fd, interest, handler); }
    void modify(int fd, unsigned interest, IoHandler* handler) {
        m_poller.modify(fd, interest, handler);
    }
    void remove(int fd) { m_poller.remove(fd); }

    TimerWheel::TimerId run_after(std::uint64_t delay_ms, TimerWheel::Callback callback) {
        return m_timers.schedule(delay_ms, std::move(callback));
    }
    void cancel_timer(TimerWheel::TimerId id) { m_timers.cancel(id); }

    // The time at the start of this iteration: one clock read serves every
    // handler in the batch.
    std::uint64_t now_ms() const { return m_now_ms; }

private:
    class Waker final : public IoHandler {
    public:
        explicit Waker(EventLoop& loop) : m_loop(loop) {}
        void on_io(bool, bool, bool) override { m_loop.drain_wakeups(); }

    private:
        EventLoop& m_loop;
    };

    void wake();
    void drain_wakeups();
    void run_posted_tasks();
    void run_deferred();
    int poll_timeout();

    Poller m_poller;
    TimerWheel m_timers;
    Waker m_waker;
    UniqueFd m_wake_read;
    UniqueFd m_wake_write;  // Same descriptor as m_wake_read when backed by eventfd.

    std::atomic<std::thread::id> m_thread_id{};
    std::atomic<bool> m_stopping{false};
    std::uint64_t m_now_ms = 0;

    std::mutex m_mutex;
    std::vector<Task> m_incoming;  // Guarded by m_mutex.
    bool m_wake_pending = false;   // Guarded by m_mutex.
    std::atomic<std::size_t> m_queue_depth{0};

    std::vector<Task> m_running;  // Loop thread only; reused to avoid reallocating.
    std::vector<Task> m_deferred;
    std::array<PollEvent, 512> m_events;
};

}  // namespace orbit::net
