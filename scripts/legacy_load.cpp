// Load generator for the original unframed server, kept so the before-and-after
// comparison in docs/BENCHMARKS.md can be reproduced. That server has no framing and
// merges or splits writes freely, so this parses newline-terminated payloads out of
// the byte stream rather than trusting message boundaries.
//
//   c++ -std=c++20 -O2 -pthread scripts/legacy_load.cpp -o legacy_load
//   ./legacy_load <port> <clients> <seconds> <messages per second> [stalled clients]
//
// Paced deliberately: flooding that server only measures how it collapses, whereas
// the fair baseline is the highest rate it can keep up with. A rate is sustained
// when delivered/s reaches published/s times (clients - 1). Stalled clients connect
// and then never read, which is what a frozen laptop on bad Wi-Fi looks like.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

constexpr std::size_t kPayloadSize = 128;  // Including the newline.
constexpr std::size_t kStampSize = 16;

std::string make_payload(std::uint64_t stamp) {
    char hex[kStampSize + 1];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(stamp));
    return std::string(kPayloadSize - kStampSize - 1, 'x') + hex + "\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: %s <port> <clients> <seconds> <rate> [stalled]\n", argv[0]);
        return 2;
    }
    const auto port = static_cast<std::uint16_t>(std::atoi(argv[1]));
    const int clients = std::atoi(argv[2]);
    const int seconds = std::atoi(argv[3]);
    const double rate = std::atof(argv[4]);
    const int stalled = argc > 5 ? std::atoi(argv[5]) : 0;

    std::vector<int> fds;
    for (int i = 0; i < clients + stalled; ++i) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            std::perror("connect");
            return 1;
        }
        const std::string name = "u" + std::to_string(i);
        ::send(fd, name.data(), name.size(), MSG_NOSIGNAL);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));  // Keep the name a lone read.
        const int enabled = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        fds.push_back(fd);
    }
    fds.resize(static_cast<std::size_t>(clients));  // Stalled sockets stay open, unread.

    std::atomic<bool> counting{false};
    std::atomic<bool> running{true};
    std::atomic<std::uint64_t> delivered{0};
    std::atomic<std::uint64_t> attributed{0};
    std::atomic<std::uint64_t> sent{0};
    std::mutex samples_mutex;
    std::vector<std::uint64_t> samples;

    constexpr int kThreads = 3;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            std::vector<pollfd> polled;
            for (std::size_t i = static_cast<std::size_t>(t); i < fds.size(); i += kThreads) {
                polled.push_back({fds[i], POLLIN, 0});
            }
            std::vector<char> buffer(64 * 1024);
            std::vector<std::uint64_t> local_samples;
            const auto start = now_ns();
            std::uint64_t issued = 0;
            std::size_t next = 0;
            std::uint64_t local_lines = 0;
            std::uint64_t local_attributed = 0;
            while (running) {
                ::poll(polled.data(), polled.size(), 0);
                for (pollfd& entry : polled) {
                    if ((entry.revents & POLLIN) == 0) {
                        continue;
                    }
                    const ssize_t count = ::recv(entry.fd, buffer.data(), buffer.size(), 0);
                    if (count <= 0 || !counting) {
                        continue;
                    }
                    const std::string_view bytes(buffer.data(), static_cast<std::size_t>(count));
                    // Lines cut by a read boundary are skipped: this is only a sample.
                    std::size_t line_start = bytes.find('\n');
                    if (line_start == std::string_view::npos) {
                        continue;
                    }
                    ++line_start;
                    for (;;) {
                        const std::size_t end = bytes.find('\n', line_start);
                        if (end == std::string_view::npos) {
                            break;
                        }
                        const std::string_view line = bytes.substr(line_start, end - line_start);
                        ++local_lines;
                        // The server prefixes "[timestamp] name: " to what it broadcasts.
                        if (line.find("] u") != std::string_view::npos) {
                            ++local_attributed;
                        }
                        if ((local_lines & 63U) == 0 && line.size() >= kStampSize) {
                            const std::string stamp(line.substr(line.size() - kStampSize));
                            const std::uint64_t sent_ns = std::strtoull(stamp.c_str(), nullptr, 16);
                            const std::uint64_t arrival = now_ns();
                            // A chunk split can cut through the stamp, leaving garbage.
                            if (sent_ns >= start && sent_ns <= arrival) {
                                local_samples.push_back(arrival - sent_ns);
                            }
                        }
                        line_start = end + 1;
                    }
                }
                const auto owed = static_cast<std::uint64_t>(static_cast<double>(now_ns() - start) /
                                                             1e9 * rate / kThreads);
                while (issued < owed) {
                    // Stamped with the scheduled time, as orbit_bench does.
                    const auto due =
                        start + static_cast<std::uint64_t>(static_cast<double>(issued) * 1e9 /
                                                           (rate / kThreads));
                    ++issued;
                    next = (next + 1) % polled.size();
                    const std::string payload = make_payload(due);
                    if (::send(polled[next].fd, payload.data(), payload.size(), MSG_NOSIGNAL) > 0 &&
                        counting) {
                        ++sent;
                    }
                }
            }
            delivered += local_lines;
            attributed += local_attributed;
            const std::lock_guard lock(samples_mutex);
            samples.insert(samples.end(), local_samples.begin(), local_samples.end());
        });
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));
    counting = true;
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    counting = false;
    running = false;
    for (auto& thread : threads) {
        thread.join();
    }

    std::sort(samples.begin(), samples.end());
    const auto percentile = [&](double percent) {
        if (samples.empty()) {
            return 0.0;
        }
        const auto index =
            static_cast<std::size_t>(static_cast<double>(samples.size() - 1) * percent / 100.0);
        return static_cast<double>(samples[index]) / 1e6;
    };
    const auto lines = delivered.load();
    std::printf(
        "published %.0f msg/s, delivered %.0f msg/s, attributed %.1f%%, p50 %.2f ms, p99 %.2f ms\n",
        static_cast<double>(sent.load()) / seconds, static_cast<double>(lines) / seconds,
        lines == 0 ? 0.0
                   : 100.0 * static_cast<double>(attributed.load()) / static_cast<double>(lines),
        percentile(50), percentile(99));
    return 0;
}
