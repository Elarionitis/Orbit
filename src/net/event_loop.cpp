#include "net/event_loop.h"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <system_error>
#include <utility>

#include "common/clock.h"

#ifdef __linux__
#include <sys/eventfd.h>
#endif

namespace orbit::net {
namespace {

constexpr std::uint64_t kTimerTickMs = 10;
constexpr std::size_t kTimerSlots = 4096;  // About 40 seconds per revolution.
constexpr int kMaxPollMs = 60'000;

}  // namespace

EventLoop::EventLoop()
    : m_timers(kTimerTickMs, kTimerSlots, monotonic_ms()),
      m_waker(*this),
      m_now_ms(monotonic_ms()) {
#ifdef __linux__
    const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "eventfd");
    }
    m_wake_read.reset(fd);
#else
    std::array<int, 2> fds{};
    if (::pipe(fds.data()) < 0) {
        throw std::system_error(errno, std::generic_category(), "pipe");
    }
    m_wake_read.reset(fds[0]);
    m_wake_write.reset(fds[1]);
    set_nonblocking(fds[0]);
    set_nonblocking(fds[1]);
    set_close_on_exec(fds[0]);
    set_close_on_exec(fds[1]);
#endif
    m_poller.add(m_wake_read.get(), kInterestRead, &m_waker);
}

EventLoop::~EventLoop() {
    m_poller.remove(m_wake_read.get());
}

void EventLoop::run() {
    m_thread_id.store(std::this_thread::get_id());
    while (!m_stopping.load(std::memory_order_acquire)) {
        const int count = m_poller.wait(m_events, poll_timeout());
        m_now_ms = monotonic_ms();
        for (int i = 0; i < count; ++i) {
            const PollEvent& event = m_events[static_cast<std::size_t>(i)];
            static_cast<IoHandler*>(event.user)
                ->on_io(event.readable, event.writable, event.failed);
        }
        run_posted_tasks();
        m_timers.advance(m_now_ms);
        run_deferred();
    }
    // Finish what was already accepted, so a task posted just before stop() is
    // not silently lost.
    run_posted_tasks();
    run_deferred();
}

void EventLoop::stop() {
    m_stopping.store(true, std::memory_order_release);
    wake();
}

void EventLoop::post(Task task) {
    // Counted before the task becomes visible. Counting afterwards would let the
    // loop run it and subtract first, wrapping the unsigned depth to a huge value
    // that load shedding would read as a full queue.
    m_queue_depth.fetch_add(1, std::memory_order_relaxed);
    bool need_wake = false;
    {
        const std::lock_guard lock(m_mutex);
        m_incoming.push_back(std::move(task));
        need_wake = !std::exchange(m_wake_pending, true);
    }
    // Only the first post of a batch pays for a syscall: the loop will pick up
    // anything that lands before it drains the queue.
    if (need_wake) {
        wake();
    }
}

void EventLoop::wake() {
#ifdef __linux__
    const std::uint64_t one = 1;
    [[maybe_unused]] const auto written = ::write(m_wake_read.get(), &one, sizeof(one));
#else
    const char byte = 0;
    [[maybe_unused]] const auto written = ::write(m_wake_write.get(), &byte, 1);
#endif
}

void EventLoop::drain_wakeups() {
    std::array<char, 64> scratch;
    while (::read(m_wake_read.get(), scratch.data(), scratch.size()) > 0) {
    }
}

void EventLoop::run_posted_tasks() {
    {
        const std::lock_guard lock(m_mutex);
        m_running.swap(m_incoming);
        m_wake_pending = false;
    }
    m_queue_depth.fetch_sub(m_running.size(), std::memory_order_relaxed);
    for (Task& task : m_running) {
        task();
    }
    m_running.clear();
}

void EventLoop::run_deferred() {
    // Indexed, because a deferred task may defer more work.
    for (std::size_t i = 0; i < m_deferred.size(); ++i) {
        Task task = std::move(m_deferred[i]);
        task();
    }
    m_deferred.clear();
}

int EventLoop::poll_timeout() {
    const auto wait = m_timers.next_deadline_in(monotonic_ms());
    if (!wait.has_value()) {
        return kMaxPollMs;
    }
    return static_cast<int>(std::min<std::uint64_t>(*wait, kMaxPollMs));
}

}  // namespace orbit::net
