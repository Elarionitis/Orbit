#pragma once

#include <sys/socket.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

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

struct SocketAddress {
    sockaddr_storage storage{};
    socklen_t length = 0;
};

// Resolves a host to every address it has, in the system's preferred order. This
// blocks on DNS, so a reactor that must not stall should not call it on a hot path.
std::vector<SocketAddress> resolve_tcp(const std::string& host, std::uint16_t port);

// Starts a non-blocking connect. The socket becomes writable when it completes;
// check connect_result() then. Throws std::system_error on immediate failure.
//
// A hostname can resolve to several addresses, and an asynchronous failure only
// shows up later, so callers that care should walk resolve_tcp() themselves.
UniqueFd connect_tcp(const SocketAddress& address);

// Connects to the first address of `host`.
UniqueFd connect_tcp(const std::string& host, std::uint16_t port);

// The pending error on a connecting socket: zero means connected.
int connect_result(int fd);

std::uint16_t local_port(int fd);
std::string peer_address(int fd);

}  // namespace orbit::net
