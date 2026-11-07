#include "common/log.h"

#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <ctime>

namespace orbit::log {
namespace {

std::atomic<Level> g_level{Level::Info};

constexpr std::array<std::string_view, 4> kNames = {"DEBUG", "INFO", "WARN", "ERROR"};

std::string timestamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t seconds = system_clock::to_time_t(now);
    const auto millis = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;

    std::tm utc{};
    gmtime_r(&seconds, &utc);
    std::array<char, 32> text{};
    const std::size_t length = std::strftime(text.data(), text.size(), "%Y-%m-%dT%H:%M:%S", &utc);
    return std::format("{}.{:03}Z", std::string_view(text.data(), length), millis);
}

}  // namespace

void set_level(Level level) {
    g_level.store(level, std::memory_order_relaxed);
}

Level level() {
    return g_level.load(std::memory_order_relaxed);
}

bool parse_level(std::string_view text, Level& out) {
    if (text == "debug") {
        out = Level::Debug;
    } else if (text == "info") {
        out = Level::Info;
    } else if (text == "warn") {
        out = Level::Warn;
    } else if (text == "error") {
        out = Level::Error;
    } else if (text == "off") {
        out = Level::Off;
    } else {
        return false;
    }
    return true;
}

void write(Level level, std::string_view message) {
    const auto name = kNames[static_cast<std::size_t>(level)];
    const std::string line = std::format("{} {:<5} {}\n", timestamp(), name, message);
    // A single write() on stderr keeps each line atomic without taking a lock.
    [[maybe_unused]] const auto written = ::write(STDERR_FILENO, line.data(), line.size());
}

}  // namespace orbit::log
