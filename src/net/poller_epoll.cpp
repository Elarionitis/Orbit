#include <sys/epoll.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <system_error>

#include "net/poller.h"

namespace orbit::net {
namespace {

constexpr int kMaxBatch = 512;

std::uint32_t to_epoll(unsigned interest) {
    std::uint32_t mask = 0;
    if ((interest & kInterestRead) != 0) {
        mask |= EPOLLIN | EPOLLRDHUP;
    }
    if ((interest & kInterestWrite) != 0) {
        mask |= EPOLLOUT;
    }
    return mask;
}

void control(int epoll_fd, int operation, int fd, unsigned interest, void* user) {
    epoll_event event{};
    event.events = to_epoll(interest);
    event.data.ptr = user;
    if (::epoll_ctl(epoll_fd, operation, fd, &event) < 0) {
        throw std::system_error(errno, std::generic_category(), "epoll_ctl");
    }
}

}  // namespace

Poller::Poller() : m_fd(::epoll_create1(EPOLL_CLOEXEC)) {
    if (!m_fd.valid()) {
        throw std::system_error(errno, std::generic_category(), "epoll_create1");
    }
}

void Poller::add(int fd, unsigned interest, void* user) {
    control(m_fd.get(), EPOLL_CTL_ADD, fd, interest, user);
}

void Poller::modify(int fd, unsigned interest, void* user) {
    control(m_fd.get(), EPOLL_CTL_MOD, fd, interest, user);
}

void Poller::remove(int fd) {
    // Failure means the descriptor is already gone, which is the desired state.
    ::epoll_ctl(m_fd.get(), EPOLL_CTL_DEL, fd, nullptr);
}

int Poller::wait(std::span<PollEvent> events, int timeout_ms) {
    std::array<epoll_event, kMaxBatch> raw;
    const int capacity = static_cast<int>(std::min<std::size_t>(events.size(), raw.size()));
    const int count = ::epoll_wait(m_fd.get(), raw.data(), capacity, timeout_ms);
    if (count < 0) {
        if (errno == EINTR) {
            return 0;
        }
        throw std::system_error(errno, std::generic_category(), "epoll_wait");
    }
    for (int i = 0; i < count; ++i) {
        const auto index = static_cast<std::size_t>(i);
        const std::uint32_t mask = raw[index].events;
        events[index].user = raw[index].data.ptr;
        events[index].readable = (mask & (EPOLLIN | EPOLLRDHUP)) != 0;
        events[index].writable = (mask & EPOLLOUT) != 0;
        events[index].failed = (mask & (EPOLLERR | EPOLLHUP)) != 0;
    }
    return count;
}

}  // namespace orbit::net
