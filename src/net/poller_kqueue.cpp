#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <system_error>

#include "net/poller.h"

namespace orbit::net {
namespace {

constexpr int kMaxBatch = 512;

void change(int queue, int fd, unsigned interest, void* user, bool adding) {
    // kqueue tracks read and write as separate filters, so toggling interest is
    // an enable or disable of each rather than a single combined mask.
    std::array<struct kevent, 2> changes{};
    const unsigned short base = adding ? EV_ADD : 0;
    const unsigned short read_flag = (interest & kInterestRead) != 0 ? EV_ENABLE : EV_DISABLE;
    const unsigned short write_flag = (interest & kInterestWrite) != 0 ? EV_ENABLE : EV_DISABLE;
    EV_SET(&changes[0], fd, EVFILT_READ, base | read_flag, 0, 0, user);
    EV_SET(&changes[1], fd, EVFILT_WRITE, base | write_flag, 0, 0, user);
    if (::kevent(queue, changes.data(), 2, nullptr, 0, nullptr) < 0) {
        throw std::system_error(errno, std::generic_category(), "kevent");
    }
}

}  // namespace

Poller::Poller() : m_fd(::kqueue()) {
    if (!m_fd.valid()) {
        throw std::system_error(errno, std::generic_category(), "kqueue");
    }
    set_close_on_exec(m_fd.get());
}

void Poller::add(int fd, unsigned interest, void* user) {
    change(m_fd.get(), fd, interest, user, true);
}

void Poller::modify(int fd, unsigned interest, void* user) {
    change(m_fd.get(), fd, interest, user, false);
}

void Poller::remove(int fd) {
    std::array<struct kevent, 2> changes{};
    EV_SET(&changes[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    EV_SET(&changes[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    // Failure means the descriptor is already gone, which is the desired state.
    ::kevent(m_fd.get(), changes.data(), 2, nullptr, 0, nullptr);
}

int Poller::wait(std::span<PollEvent> events, int timeout_ms) {
    std::array<struct kevent, kMaxBatch> raw;
    timespec timeout{};
    timespec* timeout_ptr = nullptr;
    if (timeout_ms >= 0) {
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1'000'000L;
        timeout_ptr = &timeout;
    }
    const int capacity = static_cast<int>(std::min<std::size_t>(events.size(), raw.size()));
    const int count = ::kevent(m_fd.get(), nullptr, 0, raw.data(), capacity, timeout_ptr);
    if (count < 0) {
        if (errno == EINTR) {
            return 0;
        }
        throw std::system_error(errno, std::generic_category(), "kevent");
    }
    for (int i = 0; i < count; ++i) {
        const auto index = static_cast<std::size_t>(i);
        const struct kevent& event = raw[index];
        events[index].user = event.udata;
        events[index].readable = event.filter == EVFILT_READ;
        events[index].writable = event.filter == EVFILT_WRITE;
        events[index].failed = (event.flags & EV_ERROR) != 0;
    }
    return count;
}

}  // namespace orbit::net
