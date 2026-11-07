#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

#include "net/secure_channel.h"
#include "server/config.h"
#include "server/directory.h"
#include "server/server_metrics.h"
#include "storage/wal.h"

namespace orbit::net {
class TlsContext;
}

namespace orbit::server {

class Shard;

// What shards share. The config is immutable once running, and everything else
// is either thread-safe or only reached through a shard's task queue.
struct ServerContext {
    const ServerConfig& config;
    ServerMetrics& metrics;
    NickRegistry& nicks;
    RoomDirectory& directory;
    storage::Wal* wal;  // Null when running without persistence.
    net::TlsContext* tls;
    std::vector<std::unique_ptr<Shard>>& shards;
    std::atomic<std::uint32_t>& next_room_id;
};

}  // namespace orbit::server
