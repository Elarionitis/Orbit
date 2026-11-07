#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "net/connection.h"
#include "net/listener.h"

namespace orbit::server {

// A deliberately tiny HTTP/1.0 responder for operational endpoints: GET /metrics
// and GET /healthz. Runs on an event loop; one short request per connection.
class AdminHttp {
public:
    using RenderMetrics = std::function<std::string()>;

    AdminHttp(net::EventLoop& loop, const std::string& host, std::uint16_t port,
              RenderMetrics render);

    std::uint16_t port() const { return m_listener.port(); }

    // Loop thread only.
    void start() { m_listener.start(); }
    void stop();

private:
    void on_accept(net::UniqueFd fd);
    void on_request(std::uint64_t id, ByteBuffer& input);

    net::EventLoop& m_loop;
    RenderMetrics m_render;
    net::Listener m_listener;
    std::uint64_t m_next_id = 1;
    std::map<std::uint64_t, std::unique_ptr<net::Connection>> m_connections;
};

}  // namespace orbit::server
