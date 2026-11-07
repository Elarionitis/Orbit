#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>

#include <cstdio>
#include <exception>
#include <format>
#include <thread>

#include "common/log.h"
#include "server/chat_server.h"

namespace {

// Blocks the shutdown signals in this thread, and so in every thread it spawns.
// The main thread then collects them synchronously with sigwait(): no handler, no
// async-signal-safety rules, and shutdown runs as ordinary code.
sigset_t block_shutdown_signals() {
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    return signals;
}

// A server that holds one descriptor per client needs more than the default soft
// limit, which is often only 256 or 1024.
void raise_fd_limit() {
    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) == 0) {
        limit.rlim_cur = limit.rlim_max;
        if (::setrlimit(RLIMIT_NOFILE, &limit) != 0) {
            limit.rlim_cur = 10'240;  // The ceiling macOS enforces without tuning.
            ::setrlimit(RLIMIT_NOFILE, &limit);
        }
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    using namespace orbit;

    auto parsed = server::parse_args(argc, argv);
    if (!parsed.config) {
        std::fputs(parsed.message.c_str(), parsed.exit_code == 0 ? stdout : stderr);
        return parsed.exit_code;
    }
    const server::ServerConfig& config = *parsed.config;
    log::set_level(config.log_level);

    // Belt and braces: socket writes already opt out, but OpenSSL or a future
    // code path must not be able to take the process down either.
    ::signal(SIGPIPE, SIG_IGN);
    raise_fd_limit();
    const sigset_t signals = block_shutdown_signals();

    try {
        server::ChatServer server(config);
        server.start();

        log::info("orbit {} listening on {}:{} (threads={}, tls={}, persistence={})", ORBIT_VERSION,
                  config.host, server.port(),
                  config.threads == 0 ? std::thread::hardware_concurrency() : config.threads,
                  config.tls_cert.empty() ? "off" : "on",
                  config.data_dir.empty() ? "off" : storage::to_string(config.sync_policy));
        if (config.metrics_port) {
            log::info("metrics on http://{}:{}/metrics", config.metrics_host,
                      server.metrics_port());
        }

        int received = 0;
        sigwait(&signals, &received);
        log::info("received {}, shutting down", received == SIGINT ? "SIGINT" : "SIGTERM");
        server.stop();
    } catch (const std::exception& error) {
        log::error("fatal: {}", error.what());
        return 1;
    }
    return 0;
}
