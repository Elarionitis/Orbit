#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "common/log.h"
#include "storage/wal.h"

namespace orbit::server {

struct ServerConfig {
    // Network.
    std::string host = "127.0.0.1";
    std::uint16_t port = 8080;
    std::size_t threads = 0;  // Zero means one per hardware thread.
    std::optional<std::uint16_t> metrics_port;
    std::string metrics_host = "127.0.0.1";

    // Security.
    std::string tls_cert;
    std::string tls_key;
    std::string auth_token;

    // Persistence. An empty data directory keeps everything in memory.
    std::filesystem::path data_dir;
    storage::SyncPolicy sync_policy = storage::SyncPolicy::Interval;
    std::uint64_t sync_interval_ms = 100;
    std::uint64_t wal_segment_bytes = 64ULL * 1024 * 1024;
    std::size_t wal_max_segments = 0;

    // Limits.
    std::size_t max_connections = 100'000;
    std::size_t max_rooms = 10'000;
    std::size_t max_rooms_per_session = 64;
    std::size_t max_frame_size = 64 * 1024;
    std::size_t max_body_size = 16 * 1024;
    std::size_t max_queued_bytes = 8 * 1024 * 1024;
    std::size_t max_shard_queue = 1'000'000;
    std::size_t history_messages = 1000;
    std::size_t history_bytes = 1024 * 1024;
    double rate_per_second = 50.0;  // Zero disables rate limiting.
    double rate_burst = 100.0;

    // Timing.
    std::uint64_t heartbeat_ms = 15'000;
    std::uint64_t idle_timeout_ms = 45'000;
    std::uint64_t handshake_timeout_ms = 5'000;
    std::uint64_t shutdown_grace_ms = 2'000;

    log::Level log_level = log::Level::Info;
};

// Result of parsing the command line: either a config to run, or text to print
// followed by an exit.
struct ParsedArgs {
    std::optional<ServerConfig> config;
    std::string message;
    int exit_code = 0;
};

ParsedArgs parse_args(int argc, const char* const* argv);

// Reports a contradiction between settings, or nullopt if the config is sound.
std::optional<std::string> validate(const ServerConfig& config);

}  // namespace orbit::server
