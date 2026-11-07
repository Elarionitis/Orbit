#include "server/admin_http.h"

#include <format>
#include <string_view>
#include <utility>

namespace orbit::server {
namespace {

constexpr std::size_t kMaxRequestSize = 8 * 1024;

net::SharedBytes response(std::string_view status, std::string_view content_type,
                          std::string_view body) {
    const std::string text = std::format(
        "HTTP/1.0 {}\r\nContent-Type: {}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
        status, content_type, body.size(), body);
    return std::make_shared<const std::vector<std::uint8_t>>(text.begin(), text.end());
}

}  // namespace

AdminHttp::AdminHttp(net::EventLoop& loop, const std::string& host, std::uint16_t port,
                     RenderMetrics render)
    : m_loop(loop),
      m_render(std::move(render)),
      m_listener(loop, host, port, 64, [this](net::UniqueFd fd) { on_accept(std::move(fd)); }) {}

void AdminHttp::stop() {
    m_listener.stop();
    for (auto& [id, connection] : m_connections) {
        connection->close();
    }
}

void AdminHttp::on_accept(net::UniqueFd fd) {
    const std::uint64_t id = m_next_id++;
    net::Connection::Handlers handlers;
    handlers.on_data = [this, id](ByteBuffer& input) { on_request(id, input); };
    handlers.on_close = [this, id](net::CloseReason) { m_connections.erase(id); };

    net::Connection::Options options;
    options.max_queued_bytes = 16 * 1024 * 1024;
    auto connection = std::make_unique<net::Connection>(m_loop, std::move(fd), options,
                                                        std::move(handlers));
    net::Connection* raw = connection.get();
    m_connections.emplace(id, std::move(connection));
    raw->start();
}

void AdminHttp::on_request(std::uint64_t id, ByteBuffer& input) {
    net::Connection& connection = *m_connections.at(id);
    const auto bytes = input.readable();
    const std::string_view request(reinterpret_cast<const char*>(bytes.data()), bytes.size());

    if (request.find("\r\n\r\n") == std::string_view::npos) {
        if (request.size() > kMaxRequestSize) {
            connection.send(response("431 Request Header Fields Too Large", "text/plain", ""));
            connection.close_after_flush();
        }
        return;  // Wait for the rest of the headers.
    }

    if (request.starts_with("GET /metrics ")) {
        connection.send(response("200 OK", "text/plain; version=0.0.4", m_render()));
    } else if (request.starts_with("GET /healthz ")) {
        connection.send(response("200 OK", "text/plain", "ok\n"));
    } else {
        connection.send(response("404 Not Found", "text/plain", "not found\n"));
    }
    input.clear();
    connection.close_after_flush();
}

}  // namespace orbit::server
