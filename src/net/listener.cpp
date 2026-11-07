#include "net/listener.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include "common/log.h"

namespace orbit::net {
namespace {

constexpr int kMaxAcceptsPerEvent = 64;

UniqueFd open_spare_fd() {
    return UniqueFd(::open("/dev/null", O_RDONLY | O_CLOEXEC));
}

}  // namespace

Listener::Listener(EventLoop& loop, const std::string& host, std::uint16_t port, int backlog,
                   OnAccept on_accept)
    : m_loop(loop),
      m_fd(listen_tcp(host, port, backlog)),
      m_spare_fd(open_spare_fd()),
      m_port(local_port(m_fd.get())),
      m_on_accept(std::move(on_accept)) {}

Listener::~Listener() {
    if (m_registered) {
        m_loop.remove(m_fd.get());
    }
}

void Listener::start() {
    m_loop.add(m_fd.get(), kInterestRead, this);
    m_registered = true;
}

void Listener::stop() {
    if (m_registered) {
        m_loop.remove(m_fd.get());
        m_registered = false;
    }
    m_fd.reset();
}

void Listener::on_io(bool, bool, bool) {
    for (int i = 0; i < kMaxAcceptsPerEvent && m_fd.valid(); ++i) {
        UniqueFd client(::accept(m_fd.get(), nullptr, nullptr));
        if (client.valid()) {
            set_close_on_exec(client.get());
            m_on_accept(std::move(client));
            continue;
        }
        switch (errno) {
            case EAGAIN:
#if EAGAIN != EWOULDBLOCK
            case EWOULDBLOCK:
#endif
                return;
            case EINTR:
            case ECONNABORTED:
                continue;
            case EMFILE:
            case ENFILE:
                // Out of descriptors. The pending connection stays in the backlog
                // and level-triggered polling would spin on it forever, so release
                // the spare descriptor to accept it and turn it away.
                log::warn("accept: out of file descriptors, shedding a connection");
                m_spare_fd.reset();
                client.reset(::accept(m_fd.get(), nullptr, nullptr));
                client.reset();
                m_spare_fd = open_spare_fd();
                return;
            default:
                log::error("accept failed: {}", std::strerror(errno));
                return;
        }
    }
}

}  // namespace orbit::net
