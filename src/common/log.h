#pragma once

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace orbit::log {

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3, Off = 4 };

void set_level(Level level);
Level level();
bool parse_level(std::string_view text, Level& out);

// Writes one complete line, so output from concurrent threads never interleaves.
void write(Level level, std::string_view message);

template <typename... Args>
void emit(Level at, std::format_string<Args...> format, Args&&... args) {
    if (at >= level()) {
        write(at, std::format(format, std::forward<Args>(args)...));
    }
}

template <typename... Args>
void debug(std::format_string<Args...> format, Args&&... args) {
    emit(Level::Debug, format, std::forward<Args>(args)...);
}
template <typename... Args>
void info(std::format_string<Args...> format, Args&&... args) {
    emit(Level::Info, format, std::forward<Args>(args)...);
}
template <typename... Args>
void warn(std::format_string<Args...> format, Args&&... args) {
    emit(Level::Warn, format, std::forward<Args>(args)...);
}
template <typename... Args>
void error(std::format_string<Args...> format, Args&&... args) {
    emit(Level::Error, format, std::forward<Args>(args)...);
}

}  // namespace orbit::log
