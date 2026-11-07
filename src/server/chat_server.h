#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/metrics.h"
#include "net/listener.h"
#include "server/admin_http.h"
#include "server/config.h"
#include "server/directory.h"
#include "server/server_context.h"
#include "server/server_metrics.h"
#include "server/shard.h"
#include "storage/wal.h"

namespace orbit::server {

// Owns the whole server: recovery, the shards, the listener and shutdown.
class ChatServer {
public:
    explicit ChatServer(ServerConfig config);
    ~ChatServer();

    ChatServer(const ChatServer&) = delete;
    ChatServer& operator=(const ChatServer&) = delete;

    // Recovers state, binds and starts serving. Throws if the server cannot start.
    void start();

    // Graceful shutdown: stop accepting, tell clients, drain, flush the log.
    // Idempotent.
    void stop();

    std::uint16_t port() const { return m_port; }
    std::uint16_t metrics_port() const { return m_metrics_port; }

    metrics::Registry& registry() { return m_registry; }
    const storage::RecoveryReport& recovery() const { return m_recovery; }

private:
    void recover();
    void on_accept(net::UniqueFd fd);
    std::string render_metrics();

    ServerConfig m_config;
    metrics::Registry m_registry;
    ServerMetrics m_metrics;
    NickRegistry m_nicks;
    RoomDirectory m_directory;
    std::atomic<std::uint32_t> m_next_room_id{1};
    std::shared_ptr<net::TlsContext> m_tls;
    std::unique_ptr<storage::Wal> m_wal;
    storage::RecoveryReport m_recovery;

    std::vector<std::unique_ptr<Shard>> m_shards;
    std::unique_ptr<ServerContext> m_context;
    std::vector<metrics::Gauge*> m_queue_depth_gauges;

    std::unique_ptr<net::Listener> m_listener;
    std::unique_ptr<AdminHttp> m_admin;
    std::uint16_t m_port = 0;
    std::uint16_t m_metrics_port = 0;
    std::size_t m_next_shard = 0;

    std::mutex m_lifecycle_mutex;
    bool m_running = false;
};

}  // namespace orbit::server
