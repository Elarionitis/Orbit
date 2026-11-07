#include "net/socket.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <format>
#include <memory>
#include <stdexcept>
#include <system_error>

namespace orbit::net {
namespace {

[[noreturn]] void throw_errno(const std::string& what) {
    throw std::system_error(errno, std::generic_category(), what);
}

struct AddrInfoDeleter {
    void operator()(addrinfo* info) const { freeaddrinfo(info); }
};
using AddrInfoPtr = std::unique_ptr<addrinfo, AddrInfoDeleter>;

AddrInfoPtr resolve(const std::string& host, std::uint16_t port, bool passive) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = passive ? AI_PASSIVE : 0;

    addrinfo* results = nullptr;
    const std::string service = std::to_string(port);
    const int status =
        getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(), &hints, &results);
    if (status != 0) {
        throw std::runtime_error(std::format("cannot resolve '{}': {}", host, gai_strerror(status)));
    }
    return AddrInfoPtr(results);
}

UniqueFd open_socket(const addrinfo& info) {
    UniqueFd fd(::socket(info.ai_family, info.ai_socktype, info.ai_protocol));
    if (fd.valid()) {
        set_close_on_exec(fd.get());
    }
    return fd;
}

}  // namespace

void UniqueFd::reset(int fd) {
    if (m_fd >= 0) {
        ::close(m_fd);
    }
    m_fd = fd;
}

int send_flags() {
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;  // Covered by SO_NOSIGPIPE in configure_stream_socket().
#endif
}

void set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw_errno("fcntl(O_NONBLOCK)");
    }
}

void set_close_on_exec(int fd) {
    const int flags = ::fcntl(fd, F_GETFD, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        throw_errno("fcntl(FD_CLOEXEC)");
    }
}

void configure_stream_socket(int fd) {
    const int enabled = 1;
    // Fails harmlessly on non-TCP sockets, such as the socketpairs used in tests.
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
#ifdef SO_NOSIGPIPE
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
}

UniqueFd listen_tcp(const std::string& host, std::uint16_t port, int backlog) {
    const AddrInfoPtr results = resolve(host, port, true);
    int last_error = 0;
    for (const addrinfo* info = results.get(); info != nullptr; info = info->ai_next) {
        UniqueFd fd = open_socket(*info);
        if (!fd.valid()) {
            last_error = errno;
            continue;
        }
        const int enabled = 1;
        ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
        if (::bind(fd.get(), info->ai_addr, info->ai_addrlen) == 0 &&
            ::listen(fd.get(), backlog) == 0) {
            set_nonblocking(fd.get());
            return fd;
        }
        last_error = errno;
    }
    errno = last_error;
    throw_errno(std::format("cannot listen on {}:{}", host, port));
}

UniqueFd connect_tcp(const std::string& host, std::uint16_t port) {
    const AddrInfoPtr results = resolve(host, port, false);
    int last_error = 0;
    for (const addrinfo* info = results.get(); info != nullptr; info = info->ai_next) {
        UniqueFd fd = open_socket(*info);
        if (!fd.valid()) {
            last_error = errno;
            continue;
        }
        set_nonblocking(fd.get());
        configure_stream_socket(fd.get());
        if (::connect(fd.get(), info->ai_addr, info->ai_addrlen) == 0 || errno == EINPROGRESS) {
            return fd;
        }
        last_error = errno;
    }
    errno = last_error;
    throw_errno(std::format("cannot connect to {}:{}", host, port));
}

int connect_result(int fd) {
    int error = 0;
    socklen_t length = sizeof(error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0) {
        return errno;
    }
    return error;
}

std::uint16_t local_port(int fd) {
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) < 0) {
        throw_errno("getsockname");
    }
    if (address.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6*>(&address)->sin6_port);
    }
    return ntohs(reinterpret_cast<const sockaddr_in*>(&address)->sin_port);
}

std::string peer_address(int fd) {
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&address), &length) < 0) {
        return "unknown";
    }
    std::array<char, INET6_ADDRSTRLEN> text{};
    if (address.ss_family == AF_INET6) {
        const auto* v6 = reinterpret_cast<const sockaddr_in6*>(&address);
        ::inet_ntop(AF_INET6, &v6->sin6_addr, text.data(), text.size());
        return std::format("[{}]:{}", text.data(), ntohs(v6->sin6_port));
    }
    if (address.ss_family == AF_INET) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(&address);
        ::inet_ntop(AF_INET, &v4->sin_addr, text.data(), text.size());
        return std::format("{}:{}", text.data(), ntohs(v4->sin_port));
    }
    return "local";
}

}  // namespace orbit::net
