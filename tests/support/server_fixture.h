#pragma once

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

#include "common/clock.h"
#include "common/log.h"
#include "server/chat_server.h"
#include "support/temp_dir.h"
#include "support/test_client.h"

namespace orbit::testing {

// Runs a real server in-process on an ephemeral port. Clients talk to it over
// real TCP sockets.
class ServerTest : public ::testing::Test {
protected:
    ServerTest() {
        log::set_level(log::Level::Error);
        config.port = 0;
        config.threads = 4;  // Enough that rooms and sessions land on different shards.
        config.rate_per_second = 0;
    }

    void start_server() {
        server = std::make_unique<server::ChatServer>(config);
        server->start();
    }

    std::uint16_t port() const { return server->port(); }

    TestClient login(const std::string& nick) { return TestClient::login(port(), nick); }

    // Polls until the condition holds: the server's state changes asynchronously.
    static bool eventually(const std::function<bool()>& condition, int timeout_ms = 5000) {
        const std::uint64_t deadline = monotonic_ms() + static_cast<std::uint64_t>(timeout_ms);
        while (monotonic_ms() < deadline) {
            if (condition()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return condition();
    }

    server::ServerConfig config;
    std::unique_ptr<server::ChatServer> server;
};

}  // namespace orbit::testing
