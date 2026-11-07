#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace orbit::net {

// Owning file descriptor.
class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : m_fd(fd) {}
    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : m_fd(std::exchange(other.m_fd, -1)) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.m_fd, -1));
        }
        return *this;
    }

    int get() const { return m_fd; }
    bool valid() const { return m_fd >= 0; }
    int release() { return std::exchange(m_fd, -1); }
    void reset(int fd = -1);

private:
    int m_fd = -1;
};

// Flags for send(): suppresses SIGPIPE where the platform does so per call.
// Without this a peer that disconnects mid-write would kill the process.
int send_flags();

void set_nonblocking(int fd);
void set_close_on_exec(int fd);

// Applies TCP_NODELAY, plus SO_NOSIGPIPE where available. Chat traffic is many
// small latency-sensitive writes, the worst case for Nagle's algorithm.
void configure_stream_socket(int fd);

// Binds and listens, resolving `host` as IPv4 or IPv6. Throws std::system_error.
UniqueFd listen_tcp(const std::string& host, std::uint16_t port, int backlog);

// Starts a non-blocking connect. The socket becomes writable when it completes;
// check connect_result() then. Throws std::system_error on immediate failure.
UniqueFd connect_tcp(const std::string& host, std::uint16_t port);

// The pending error on a connecting socket: zero means connected.
int connect_result(int fd);

std::uint16_t local_port(int fd);
std::string peer_address(int fd);

}  // namespace orbit::net
