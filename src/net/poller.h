#pragma once

#include <cstdint>
#include <span>

#include "net/socket.h"

namespace orbit::net {

enum Interest : std::uint8_t {
    kInterestNone = 0,
    kInterestRead = 1,
    kInterestWrite = 2,
};

struct PollEvent {
    void* user = nullptr;
    bool readable = false;
    bool writable = false;
    bool failed = false;  // Error or hang-up: the next read or write reports the detail.
};

// Readiness notification over epoll (Linux) or kqueue (macOS, BSD). The backend
// is chosen at build time, so there is no virtual dispatch on the hot path.
// Registration is level-triggered on both: a handler may stop reading at any
// point without losing the wake-up for the bytes it left behind.
class Poller {
public:
    Poller();

    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    void add(int fd, unsigned interest, void* user);
    void modify(int fd, unsigned interest, void* user);
    void remove(int fd);

    // Waits up to `timeout_ms` (negative blocks) and returns the event count.
    int wait(std::span<PollEvent> events, int timeout_ms);

private:
    UniqueFd m_fd;
};

}  // namespace orbit::net
