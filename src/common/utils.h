#ifndef ORBIT_UTILS_H
#define ORBIT_UTILS_H

#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

namespace orbit {
    inline std::string get_timestamp() {
        const auto now = std::chrono::system_clock::now();
        const std::time_t time = std::chrono::system_clock::to_time_t(now);
        std::tm local_time{};
        if (localtime_r(&time, &local_time) == nullptr) {
            return {};
        }

        std::ostringstream stream;
        stream << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S");
        return stream.str();
    }

    constexpr int kBufferSize = 1024;
    constexpr uint16_t kDefaultPort = 8080;
}

#endif // ORBIT_UTILS_H
