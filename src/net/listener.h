#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "net/event_loop.h"
#include "net/socket.h"

namespace orbit::net {

// Accepts connections on an event loop and passes each socket to a callback.
class Listener final : public IoHandler {
public:
    using OnAccept = std::function<void(UniqueFd)>;

    Listener(EventLoop& loop, const std::string& host, std::uint16_t port, int backlog,
             OnAccept on_accept);
    ~Listener() override;

    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    std::uint16_t port() const { return m_port; }

    // Loop thread only.
    void start();
    void stop();

    void on_io(bool readable, bool writable, bool failed) override;

private:
    EventLoop& m_loop;
    UniqueFd m_fd;
    UniqueFd m_spare_fd;
    std::uint16_t m_port;
    OnAccept m_on_accept;
    bool m_registered = false;
};

}  // namespace orbit::net
