#include "server/chat_server.h"

#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <future>
#include <thread>
#include <utility>

#include "common/clock.h"
#include "common/log.h"
#include "protocol/messages.h"
#ifdef ORBIT_HAS_TLS
#include "net/tls.h"
#endif

namespace orbit::server {

ChatServer::ChatServer(ServerConfig config)
    : m_config(std::move(config)), m_metrics(m_registry) {
    if (m_config.threads == 0) {
        m_config.threads = std::max(1U, std::thread::hardware_concurrency());
    }
}

ChatServer::~ChatServer() {
    stop();
}

void ChatServer::start() {
    const std::lock_guard lock(m_lifecycle_mutex);
    if (m_running) {
        return;
    }

#ifdef ORBIT_HAS_TLS
    if (!m_config.tls_cert.empty()) {
        m_tls = net::TlsContext::server(m_config.tls_cert, m_config.tls_key);
    }
#endif

    m_context = std::make_unique<ServerContext>(
        ServerContext{m_config, m_metrics, m_nicks, m_directory, nullptr, m_tls.get(), m_shards,
                      m_next_room_id});
    for (std::size_t i = 0; i < m_config.threads; ++i) {
        m_shards.push_back(std::make_unique<Shard>(i, *m_context));
        m_queue_depth_gauges.push_back(&m_registry.gauge(
            "orbit_shard_queue_depth", "Tasks waiting on each shard.",
            std::format("shard=\"{}\"", i)));
    }

    if (!m_config.data_dir.empty()) {
        recover();
    }

    // Everything binds on shard 0; if a bind throws, no thread has started yet.
    net::EventLoop& first_loop = m_shards[0]->loop();
    m_listener = std::make_unique<net::Listener>(
        first_loop, m_config.host, m_config.port, 4096,
        [this](net::UniqueFd fd) { on_accept(std::move(fd)); });
    m_port = m_listener->port();
    if (m_config.metrics_port) {
        m_admin = std::make_unique<AdminHttp>(first_loop, m_config.metrics_host,
                                              *m_config.metrics_port,
                                              [this] { return render_metrics(); });
        m_metrics_port = m_admin->port();
    }

    first_loop.post([this] {
        m_listener->start();
        if (m_admin) {
            m_admin->start();
        }
    });
    for (auto& shard : m_shards) {
        shard->start();
    }
    m_running = true;
}

void ChatServer::recover() {
    const std::uint64_t start = monotonic_ms();
    const auto owner = [this](const std::string& room) -> Shard& {
        return *m_shards[room_shard_index(room, m_shards.size())];
    };

    m_recovery = storage::Wal::recover(m_config.data_dir, [&](const storage::WalRecord& record) {
        owner(record.room).restore_message(record);
    });
    for (const auto& [room, head] : m_recovery.room_heads) {
        owner(room).restore_head(room, head);
    }
    if (m_recovery.segments > 0) {
        log::info("recovered {} messages in {} rooms from {} segments in {} ms",
                  m_recovery.records, m_recovery.room_heads.size(), m_recovery.segments,
                  monotonic_ms() - start);
    }

    storage::WalOptions options;
    options.directory = m_config.data_dir;
    options.sync_policy = m_config.sync_policy;
    options.sync_interval_ms = m_config.sync_interval_ms;
    options.segment_bytes = m_config.wal_segment_bytes;
    options.max_segments = m_config.wal_max_segments;
    m_wal = std::make_unique<storage::Wal>(std::move(options), m_recovery.room_heads, &m_registry);
    m_context->wal = m_wal.get();
}

void ChatServer::on_accept(net::UniqueFd fd) {
    if (static_cast<std::size_t>(m_metrics.connections_active.value()) >=
        m_config.max_connections) {
        m_metrics.connections_rejected.add();
        // Best effort: the socket is brand new, so this small write will not block.
        const auto frame = protocol::encode(
            protocol::Error{0, protocol::ErrorCode::ServerFull, "connection limit reached"});
        [[maybe_unused]] const auto sent =
            ::send(fd.get(), frame.data(), frame.size(), net::send_flags());
        return;
    }
    Shard& shard = *m_shards[m_next_shard];
    m_next_shard = (m_next_shard + 1) % m_shards.size();
    shard.adopt(std::move(fd));
}

std::string ChatServer::render_metrics() {
    for (std::size_t i = 0; i < m_shards.size(); ++i) {
        m_queue_depth_gauges[i]->set(static_cast<std::int64_t>(m_shards[i]->loop().queue_depth()));
    }
    return m_registry.render();
}

void ChatServer::stop() {
    const std::lock_guard lock(m_lifecycle_mutex);
    if (!m_running) {
        return;
    }
    m_running = false;

    // 1. Stop accepting.
    std::promise<void> listeners_closed;
    m_shards[0]->loop().post([this, &listeners_closed] {
        m_listener->stop();
        if (m_admin) {
            m_admin->stop();
        }
        listeners_closed.set_value();
    });
    listeners_closed.get_future().wait();

    // 2. Tell every client, and give them a bounded time to drain.
    for (auto& shard : m_shards) {
        shard->begin_shutdown();
    }
    const std::uint64_t deadline = monotonic_ms() + m_config.shutdown_grace_ms;
    while (m_metrics.connections_active.value() > 0 && monotonic_ms() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // 3. Stop the loops. Tasks already queued still run, so nothing accepted is lost.
    for (auto& shard : m_shards) {
        shard->loop().stop();
    }
    for (auto& shard : m_shards) {
        shard->join_thread();
    }

    // 4. Flush the log now that nothing can append to it.
    if (m_wal) {
        m_wal->close();
    }
    log::info("shutdown complete");
}

}  // namespace orbit::server
