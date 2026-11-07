#include "server/config.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <format>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace orbit::server {
namespace {

constexpr std::string_view kUsage = R"(Usage: orbit_server [options]

Network:
  --host HOST               Address to bind (default 127.0.0.1)
  --port PORT               Port to listen on (default 8080, 0 picks a free port)
  --threads N               Event-loop threads (default: hardware threads)
  --metrics-port PORT       Serve /metrics and /healthz on this port (default: off)
  --metrics-host HOST       Address for the metrics listener (default 127.0.0.1)

Security:
  --tls-cert FILE           PEM certificate chain; enables TLS with --tls-key
  --tls-key FILE            PEM private key
  --auth-token-file FILE    Require clients to present the token in this file

Persistence:
  --data-dir DIR            Write-ahead log directory (default: in-memory only)
  --fsync POLICY            none, interval or always (default interval)
  --fsync-interval-ms N     Sync period for the interval policy (default 100)
  --wal-segment-mb N        Rotate log segments at this size (default 64)
  --wal-max-segments N      Delete the oldest segments past this count (default: keep all)

Limits:
  --max-connections N       Concurrent connections (default 100000)
  --max-rooms N             Rooms on this server (default 10000)
  --max-frame-bytes N       Largest accepted frame (default 65536)
  --max-body-bytes N        Largest message body (default 16384)
  --max-queued-bytes N      Per-client output queue before disconnecting (default 8388608)
  --history N               Messages kept per room for replay (default 1000)
  --rate N                  Messages per second per client, 0 for no limit (default 50)
  --burst N                 Rate limiter burst size (default 100)

Timing:
  --heartbeat-ms N          Client ping interval to advertise (default 15000)
  --idle-timeout-ms N       Disconnect clients silent for this long (default 45000)
  --handshake-timeout-ms N  Deadline for the client's Hello (default 5000)

Other:
  --log-level LEVEL         debug, info, warn, error or off (default info)
  --help                    Show this help
  --version                 Show the version
)";

template <typename T>
T parse_number(std::string_view flag, std::string_view text, T min, T max) {
    T value{};
    const auto* end = text.data() + text.size();
    const auto [parsed, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc{} || parsed != end || value < min || value > max) {
        throw std::invalid_argument(
            std::format("{}: expected a number from {} to {}, got '{}'", flag, min, max, text));
    }
    return value;
}

std::size_t parse_size(std::string_view flag, std::string_view text) {
    return parse_number<std::size_t>(flag, text, 0, std::numeric_limits<std::size_t>::max());
}

std::uint64_t parse_positive(std::string_view flag, std::string_view text) {
    return parse_number<std::uint64_t>(flag, text, 1, std::numeric_limits<std::uint64_t>::max());
}

std::string read_token(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::invalid_argument(std::format("cannot read token file '{}'", path));
    }
    std::string token;
    std::getline(file, token);
    while (!token.empty() && (token.back() == '\r' || token.back() == ' ')) {
        token.pop_back();
    }
    if (token.empty()) {
        throw std::invalid_argument(std::format("token file '{}' is empty", path));
    }
    return token;
}

using Setter = std::function<void(ServerConfig&, std::string_view flag, const std::string&)>;

const std::map<std::string_view, Setter>& setters() {
    static const std::map<std::string_view, Setter> table = {
        {"--host", [](auto& c, auto, const auto& v) { c.host = v; }},
        {"--port", [](auto& c, auto f,
                      const auto& v) { c.port = parse_number<std::uint16_t>(f, v, 0, 65535); }},
        {"--threads", [](auto& c, auto f,
                         const auto& v) { c.threads = parse_number<std::size_t>(f, v, 1, 1024); }},
        {"--metrics-port",
         [](auto& c, auto f, const auto& v) {
             c.metrics_port = parse_number<std::uint16_t>(f, v, 0, 65535);
         }},
        {"--metrics-host", [](auto& c, auto, const auto& v) { c.metrics_host = v; }},
        {"--tls-cert", [](auto& c, auto, const auto& v) { c.tls_cert = v; }},
        {"--tls-key", [](auto& c, auto, const auto& v) { c.tls_key = v; }},
        {"--auth-token-file", [](auto& c, auto, const auto& v) { c.auth_token = read_token(v); }},
        {"--data-dir", [](auto& c, auto, const auto& v) { c.data_dir = v; }},
        {"--fsync",
         [](auto& c, auto f, const auto& v) {
             if (v == "none") {
                 c.sync_policy = storage::SyncPolicy::None;
             } else if (v == "interval") {
                 c.sync_policy = storage::SyncPolicy::Interval;
             } else if (v == "always") {
                 c.sync_policy = storage::SyncPolicy::Always;
             } else {
                 throw std::invalid_argument(std::format("{}: unknown policy '{}'", f, v));
             }
         }},
        {"--fsync-interval-ms",
         [](auto& c, auto f, const auto& v) { c.sync_interval_ms = parse_positive(f, v); }},
        {"--wal-segment-mb",
         [](auto& c, auto f, const auto& v) {
             c.wal_segment_bytes = parse_positive(f, v) * 1024 * 1024;
         }},
        {"--wal-max-segments",
         [](auto& c, auto f, const auto& v) { c.wal_max_segments = parse_size(f, v); }},
        {"--max-connections",
         [](auto& c, auto f, const auto& v) { c.max_connections = parse_positive(f, v); }},
        {"--max-rooms", [](auto& c, auto f, const auto& v) { c.max_rooms = parse_positive(f, v); }},
        {"--max-frame-bytes",
         [](auto& c, auto f, const auto& v) { c.max_frame_size = parse_positive(f, v); }},
        {"--max-body-bytes",
         [](auto& c, auto f, const auto& v) { c.max_body_size = parse_positive(f, v); }},
        {"--max-queued-bytes",
         [](auto& c, auto f, const auto& v) { c.max_queued_bytes = parse_positive(f, v); }},
        {"--history",
         [](auto& c, auto f, const auto& v) { c.history_messages = parse_size(f, v); }},
        {"--rate",
         [](auto& c, auto f, const auto& v) {
             c.rate_per_second = static_cast<double>(parse_size(f, v));
         }},
        {"--burst",
         [](auto& c, auto f, const auto& v) {
             c.rate_burst = static_cast<double>(parse_positive(f, v));
         }},
        {"--heartbeat-ms",
         [](auto& c, auto f, const auto& v) { c.heartbeat_ms = parse_positive(f, v); }},
        {"--idle-timeout-ms",
         [](auto& c, auto f, const auto& v) { c.idle_timeout_ms = parse_positive(f, v); }},
        {"--handshake-timeout-ms",
         [](auto& c, auto f, const auto& v) { c.handshake_timeout_ms = parse_positive(f, v); }},
        {"--log-level",
         [](auto& c, auto f, const auto& v) {
             if (!log::parse_level(v, c.log_level)) {
                 throw std::invalid_argument(std::format("{}: unknown level '{}'", f, v));
             }
         }},
    };
    return table;
}

}  // namespace

ParsedArgs parse_args(int argc, const char* const* argv) {
    ParsedArgs result;
    ServerConfig config;
    try {
        for (int i = 1; i < argc; ++i) {
            std::string_view arg = argv[i];
            if (arg == "--help" || arg == "-h") {
                result.message = std::string(kUsage);
                return result;
            }
            if (arg == "--version") {
                result.message = std::format("orbit_server {}\n", ORBIT_VERSION);
                return result;
            }

            std::string value;
            const std::size_t equals = arg.find('=');
            const std::string_view flag = arg.substr(0, equals);
            const auto setter = setters().find(flag);
            if (setter == setters().end()) {
                throw std::invalid_argument(std::format("unknown option '{}'", flag));
            }
            if (equals != std::string_view::npos) {
                value = arg.substr(equals + 1);
            } else if (i + 1 < argc) {
                value = argv[++i];
            } else {
                throw std::invalid_argument(std::format("{} needs a value", flag));
            }
            setter->second(config, flag, value);
        }
        if (const auto problem = validate(config)) {
            throw std::invalid_argument(*problem);
        }
    } catch (const std::exception& error) {
        result.message = std::format("orbit_server: {}\nTry --help.\n", error.what());
        result.exit_code = 2;
        return result;
    }
    result.config = std::move(config);
    return result;
}

std::optional<std::string> validate(const ServerConfig& config) {
    if (config.tls_cert.empty() != config.tls_key.empty()) {
        return "--tls-cert and --tls-key must be given together";
    }
#ifndef ORBIT_HAS_TLS
    if (!config.tls_cert.empty()) {
        return "this build has no TLS support";
    }
#endif
    if (config.max_body_size + 512 > config.max_frame_size) {
        return "--max-frame-bytes must exceed --max-body-bytes by at least 512";
    }
    if (config.history_bytes >= config.max_queued_bytes) {
        return "--max-queued-bytes must exceed the history replay size";
    }
    if (config.idle_timeout_ms <= config.heartbeat_ms) {
        return "--idle-timeout-ms must be greater than --heartbeat-ms";
    }
    return std::nullopt;
}

}  // namespace orbit::server
