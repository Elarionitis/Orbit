// Load generator for orbit_server.
//
// Two modes:
//
//   Open loop (--rate N): messages are scheduled on a fixed timetable and latency
//   is measured from the *scheduled* time. If the server stalls, the backlog shows
//   up as latency, which avoids the coordinated omission that makes closed-loop
//   benchmarks look better than the system really is.
//
//   Closed loop (--rate 0): each publisher keeps a window of unacknowledged
//   messages in flight, to find the maximum sustainable throughput.
//
// It also audits delivery. Every client checks that sequence numbers arrive without
// gaps, and the final tally compares deliveries with acknowledged messages times
// room size, so a run reports lost messages rather than assuming there were none.
//
// Latency uses this machine's monotonic clock, so run it on the same host as the
// server, or treat the figures as relative.
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <format>
#include <memory>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "bench/latency_histogram.h"
#include "common/byte_buffer.h"
#include "common/clock.h"
#include "net/poller.h"
#include "net/socket.h"
#include "protocol/messages.h"

namespace {

using namespace orbit;
using namespace orbit::protocol;

constexpr std::string_view kUsage = R"(Usage: orbit_bench [options]

  --host HOST        Server address (default 127.0.0.1)
  --port PORT        Server port (default 8080)
  --clients N        Connections to open (default 100)
  --rooms N          Rooms to spread them across (default 10)
  --publishers N     How many clients publish (default: all)
  --rate N           Total messages per second; 0 finds the maximum (default 0)
  --window N         In-flight messages per publisher when --rate is 0 (default 16)
  --duration S       Measured seconds (default 10)
  --warmup S         Unmeasured seconds beforehand (default 2)
  --body BYTES       Message body size, at least 16 (default 128)
  --threads N        Generator threads (default 2)
  --json             Print one JSON object rather than a report
)";

struct Options {
    std::string host = "127.0.0.1";
    std::uint16_t port = 8080;
    std::size_t clients = 100;
    std::size_t rooms = 10;
    std::size_t publishers = 0;
    std::uint64_t rate = 0;
    std::size_t window = 16;
    std::uint64_t duration_s = 10;
    std::uint64_t warmup_s = 2;
    std::size_t body_size = 128;
    std::size_t threads = 2;
    bool json = false;
};

enum class Phase : int { Setup, Running, Draining, Done };

struct Shared {
    std::atomic<Phase> phase{Phase::Setup};
    std::atomic<std::size_t> joined{0};
    std::atomic<std::uint64_t> measure_start_ns{0};
    std::atomic<std::uint64_t> measure_end_ns{0};
    std::atomic<std::uint64_t> last_delivery_ms{0};
};

struct BenchClient {
    net::UniqueFd fd;
    ByteBuffer input;
    ByteBuffer output;
    std::string nick;
    std::string room;
    std::size_t room_index = 0;
    bool publisher = false;
    bool connected = false;
    bool joined = false;
    bool open = true;
    bool write_armed = false;
    std::uint64_t last_seq = 0;
    std::uint64_t next_send_ns = 0;
    std::uint64_t interval_ns = 0;
    std::size_t in_flight = 0;
};

// Encodes the send time as text, keeping the body valid UTF-8.
void stamp_body(std::string& body, std::uint64_t timestamp_ns) {
    static constexpr std::string_view kHex = "0123456789abcdef";
    for (std::size_t i = 0; i < 16; ++i) {
        body[i] = kHex[(timestamp_ns >> ((15 - i) * 4)) & 0xFU];
    }
}

std::uint64_t read_stamp(std::string_view body) {
    std::uint64_t value = 0;
    std::from_chars(body.data(), body.data() + 16, value, 16);
    return value;
}

class Worker {
public:
    Worker(const Options& options, Shared& shared) : m_options(options), m_shared(shared) {
        m_published.assign(options.rooms, 0);
        m_delivered.assign(options.rooms, 0);
        m_body.assign(options.body_size, 'x');
    }

    void add_client(std::unique_ptr<BenchClient> client) { m_clients.push_back(std::move(client)); }

    void start() {
        m_thread = std::thread([this] { run(); });
    }
    void join() { m_thread.join(); }

    const bench::LatencyHistogram& latency() const { return *m_latency; }
    const std::vector<std::uint64_t>& published() const { return m_published; }
    const std::vector<std::uint64_t>& delivered() const { return m_delivered; }
    std::uint64_t measured_published() const { return m_measured_published; }
    std::uint64_t measured_delivered() const { return m_measured_delivered; }
    std::uint64_t sequence_gaps() const { return m_gaps; }
    std::uint64_t errors() const { return m_errors; }
    std::uint64_t disconnects() const { return m_disconnects; }

private:
    void run() {
        for (auto& client : m_clients) {
            client->write_armed = true;
            m_poller.add(client->fd.get(), net::kInterestRead | net::kInterestWrite, client.get());
        }

        std::array<net::PollEvent, 256> events;
        while (m_shared.phase.load(std::memory_order_acquire) != Phase::Done) {
            const Phase phase = m_shared.phase.load(std::memory_order_acquire);
            // Spin while publishing, so pacing is not limited by a sleep's granularity.
            const int timeout = phase == Phase::Running ? 0 : 1;
            const int count = m_poller.wait(events, timeout);
            for (int i = 0; i < count; ++i) {
                const net::PollEvent& event = events[static_cast<std::size_t>(i)];
                auto* client = static_cast<BenchClient*>(event.user);
                if (client->open && !client->connected) {
                    // The socket stays silent until the connect finishes; writing
                    // to it before then fails with ENOTCONN on some platforms.
                    if (event.writable || event.failed) {
                        on_connect_finished(*client);
                    }
                    continue;
                }
                if (client->open && (event.readable || event.failed)) {
                    on_readable(*client);
                }
                if (client->open && event.writable) {
                    flush(*client);
                }
            }
            if (phase == Phase::Running) {
                publish_due();
            }
        }
    }

    void on_connect_finished(BenchClient& client) {
        if (net::connect_result(client.fd.get()) != 0) {
            drop(client);
            return;
        }
        client.connected = true;
        queue(client, Hello{kProtocolVersion, client.nick, ""});
    }

    void queue(BenchClient& client, const AnyMessage& message) {
        client.output.append(encode(message));
        flush(client);
    }

    void flush(BenchClient& client) {
        while (!client.output.empty()) {
            const auto bytes = client.output.readable();
            const ssize_t sent =
                ::send(client.fd.get(), bytes.data(), bytes.size(), net::send_flags());
            if (sent > 0) {
                client.output.consume(static_cast<std::size_t>(sent));
            } else if (sent < 0 && errno == EINTR) {
                continue;
            } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            } else {
                drop(client);
                return;
            }
        }
        const bool want_write = !client.output.empty();
        if (want_write != client.write_armed) {
            client.write_armed = want_write;
            unsigned interest = net::kInterestRead;
            if (want_write) {
                interest |= net::kInterestWrite;
            }
            m_poller.modify(client.fd.get(), interest, &client);
        }
    }

    void drop(BenchClient& client) {
        client.open = false;
        m_poller.remove(client.fd.get());
        client.fd.reset();
        ++m_disconnects;
    }

    void on_readable(BenchClient& client) {
        auto target = client.input.prepare(64 * 1024);
        const ssize_t received = ::recv(client.fd.get(), target.data(), target.size(), 0);
        if (received == 0 || (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            drop(client);
            return;
        }
        if (received < 0) {
            return;
        }
        client.input.commit(static_cast<std::size_t>(received));
        for (;;) {
            const auto frame = extract_frame(client.input.readable(), 16 * 1024 * 1024);
            if (frame.status != ExtractStatus::Complete) {
                return;
            }
            const auto message = decode(frame.body);
            client.input.consume(frame.consumed);
            if (message) {
                on_message(client, *message);
            }
        }
    }

    void on_message(BenchClient& client, const AnyMessage& message) {
        if (const auto* chat = std::get_if<Message>(&message)) {
            on_delivery(client, *chat);
        } else if (const auto* ack = std::get_if<Ack>(&message)) {
            ++m_published[client.room_index];
            if (client.in_flight > 0) {
                --client.in_flight;
            }
            const std::uint64_t sent_ns = ack->correlation;
            if (in_measure_window(sent_ns)) {
                ++m_measured_published;
            }
        } else if (std::holds_alternative<Welcome>(message)) {
            queue(client, Join{client.room, 0});
        } else if (const auto* joined = std::get_if<Joined>(&message)) {
            client.joined = true;
            client.last_seq = joined->head_seq;
            m_shared.joined.fetch_add(1, std::memory_order_acq_rel);
        } else if (const auto* error = std::get_if<Error>(&message)) {
            if (m_errors++ == 0) {
                std::fprintf(stderr, "orbit_bench: first server error: %s (%s)\n",
                             std::string(error_name(error->code)).c_str(), error->detail.c_str());
            }
        }
    }

    void on_delivery(BenchClient& client, const Message& message) {
        if (message.seq <= client.last_seq) {
            return;  // Replay of history from before the run.
        }
        if (message.seq != client.last_seq + 1) {
            ++m_gaps;
        }
        client.last_seq = message.seq;
        ++m_delivered[client.room_index];

        const std::uint64_t now = monotonic_ns();
        const std::uint64_t sent_ns = read_stamp(message.body);
        if (in_measure_window(sent_ns)) {
            ++m_measured_delivered;
            m_latency->record(now > sent_ns ? now - sent_ns : 0);
        }
        m_shared.last_delivery_ms.store(monotonic_ms(), std::memory_order_relaxed);
    }

    bool in_measure_window(std::uint64_t timestamp_ns) const {
        const std::uint64_t start = m_shared.measure_start_ns.load(std::memory_order_relaxed);
        const std::uint64_t end = m_shared.measure_end_ns.load(std::memory_order_relaxed);
        return start != 0 && timestamp_ns >= start && (end == 0 || timestamp_ns < end);
    }

    void publish_due() {
        const std::uint64_t now = monotonic_ns();
        for (auto& owned : m_clients) {
            BenchClient& client = *owned;
            if (!client.publisher || !client.open) {
                continue;
            }
            if (m_options.rate > 0) {
                // Open loop: catch up on every slot that has passed, and stamp each
                // with the time it was due, not the time it actually left.
                while (client.next_send_ns <= now && client.output.size() < 1024 * 1024) {
                    send_message(client, client.next_send_ns);
                    client.next_send_ns += client.interval_ns;
                }
            } else {
                while (client.in_flight < m_options.window && client.output.size() < 1024 * 1024) {
                    send_message(client, monotonic_ns());
                }
            }
        }
    }

    void send_message(BenchClient& client, std::uint64_t stamp_ns) {
        stamp_body(m_body, stamp_ns);
        ++client.in_flight;
        queue(client, Publish{stamp_ns, client.room, m_body});
    }

    const Options& m_options;
    Shared& m_shared;
    net::Poller m_poller;
    std::vector<std::unique_ptr<BenchClient>> m_clients;
    std::thread m_thread;
    std::string m_body;

    std::unique_ptr<bench::LatencyHistogram> m_latency =
        std::make_unique<bench::LatencyHistogram>();
    std::vector<std::uint64_t> m_published;
    std::vector<std::uint64_t> m_delivered;
    std::uint64_t m_measured_published = 0;
    std::uint64_t m_measured_delivered = 0;
    std::uint64_t m_gaps = 0;
    std::uint64_t m_errors = 0;
    std::uint64_t m_disconnects = 0;
};

template <typename T>
T to_number(std::string_view flag, std::string_view text) {
    T value{};
    const auto* end = text.data() + text.size();
    const auto [parsed, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc{} || parsed != end) {
        throw std::invalid_argument(std::format("{}: invalid number '{}'", flag, text));
    }
    return value;
}

Options parse_options(int argc, char* argv[]) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag = argv[i];
        if (flag == "--help" || flag == "-h") {
            std::fputs(std::string(kUsage).c_str(), stdout);
            std::exit(0);
        }
        if (flag == "--json") {
            options.json = true;
            continue;
        }
        if (i + 1 >= argc) {
            throw std::invalid_argument(std::format("{} needs a value", flag));
        }
        const std::string_view value = argv[++i];
        if (flag == "--host") {
            options.host = value;
        } else if (flag == "--port") {
            options.port = to_number<std::uint16_t>(flag, value);
        } else if (flag == "--clients") {
            options.clients = to_number<std::size_t>(flag, value);
        } else if (flag == "--rooms") {
            options.rooms = to_number<std::size_t>(flag, value);
        } else if (flag == "--publishers") {
            options.publishers = to_number<std::size_t>(flag, value);
        } else if (flag == "--rate") {
            options.rate = to_number<std::uint64_t>(flag, value);
        } else if (flag == "--window") {
            options.window = to_number<std::size_t>(flag, value);
        } else if (flag == "--duration") {
            options.duration_s = to_number<std::uint64_t>(flag, value);
        } else if (flag == "--warmup") {
            options.warmup_s = to_number<std::uint64_t>(flag, value);
        } else if (flag == "--body") {
            options.body_size = to_number<std::size_t>(flag, value);
        } else if (flag == "--threads") {
            options.threads = to_number<std::size_t>(flag, value);
        } else {
            throw std::invalid_argument(std::format("unknown option '{}'", flag));
        }
    }
    if (options.publishers == 0 || options.publishers > options.clients) {
        options.publishers = options.clients;
    }
    if (options.body_size < 16) {
        throw std::invalid_argument("--body must be at least 16");
    }
    if (options.clients == 0 || options.rooms == 0 || options.threads == 0) {
        throw std::invalid_argument("--clients, --rooms and --threads must be positive");
    }
    return options;
}

void raise_fd_limit() {
    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) == 0) {
        limit.rlim_cur = std::min<rlim_t>(limit.rlim_max, 1'000'000);
        if (::setrlimit(RLIMIT_NOFILE, &limit) != 0) {
            limit.rlim_cur = 10'240;  // The ceiling macOS enforces without tuning.
            ::setrlimit(RLIMIT_NOFILE, &limit);
        }
    }
}

double to_ms(std::uint64_t nanoseconds) {
    return static_cast<double>(nanoseconds) / 1e6;
}

}  // namespace

int main(int argc, char* argv[]) {
    Options options;
    try {
        options = parse_options(argc, argv);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "orbit_bench: %s\n", error.what());
        return 2;
    }
    ::signal(SIGPIPE, SIG_IGN);
    raise_fd_limit();

    Shared shared;
    std::vector<std::unique_ptr<Worker>> workers;
    for (std::size_t i = 0; i < options.threads; ++i) {
        workers.push_back(std::make_unique<Worker>(options, shared));
    }

    std::mt19937_64 random(42);
    const std::uint64_t interval_ns =
        options.rate > 0 ? 1'000'000'000ULL * options.publishers / options.rate : 0;
    std::uniform_int_distribution<std::uint64_t> stagger(0,
                                                         std::max<std::uint64_t>(interval_ns, 1));
    const std::uint64_t run_id = wall_clock_ms() % 1'000'000;

    std::vector<BenchClient*> clients;
    try {
        for (std::size_t i = 0; i < options.clients; ++i) {
            auto client = std::make_unique<BenchClient>();
            client->fd = net::connect_tcp(options.host, options.port);
            client->nick = std::format("b{}x{}", run_id, i);
            client->room_index = i % options.rooms;
            client->room = std::format("bench{}", client->room_index);
            client->publisher = i < options.publishers;
            client->interval_ns = interval_ns;
            clients.push_back(client.get());
            workers[i % workers.size()]->add_client(std::move(client));
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "orbit_bench: %s\n", error.what());
        return 1;
    }

    for (auto& worker : workers) {
        worker->start();
    }

    const std::uint64_t setup_deadline = monotonic_ms() + 30'000;
    while (shared.joined.load(std::memory_order_acquire) < options.clients) {
        if (monotonic_ms() > setup_deadline) {
            std::fprintf(stderr, "orbit_bench: only %zu of %zu clients joined\n",
                         shared.joined.load(), options.clients);
            shared.phase = Phase::Done;
            for (auto& worker : workers) {
                worker->join();
            }
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const std::uint64_t begin_ns = monotonic_ns();
    for (BenchClient* client : clients) {
        client->next_send_ns = begin_ns + (interval_ns > 0 ? stagger(random) : 0);
    }
    shared.phase.store(Phase::Running, std::memory_order_release);

    std::this_thread::sleep_for(std::chrono::seconds(options.warmup_s));
    const std::uint64_t measure_start = monotonic_ns();
    shared.measure_start_ns.store(measure_start);
    std::this_thread::sleep_for(std::chrono::seconds(options.duration_s));
    const std::uint64_t measure_end = monotonic_ns();
    shared.measure_end_ns.store(measure_end);

    // Stop publishing, then wait for the pipeline to go quiet before auditing.
    shared.phase.store(Phase::Draining, std::memory_order_release);
    shared.last_delivery_ms.store(monotonic_ms());
    while (monotonic_ms() - shared.last_delivery_ms.load() < 1000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    shared.phase.store(Phase::Done, std::memory_order_release);
    for (auto& worker : workers) {
        worker->join();
    }

    bench::LatencyHistogram latency;
    std::uint64_t published = 0;
    std::uint64_t delivered = 0;
    std::uint64_t gaps = 0;
    std::uint64_t errors = 0;
    std::uint64_t disconnects = 0;
    std::vector<std::uint64_t> room_published(options.rooms, 0);
    std::vector<std::uint64_t> room_delivered(options.rooms, 0);
    for (const auto& worker : workers) {
        latency.merge(worker->latency());
        published += worker->measured_published();
        delivered += worker->measured_delivered();
        gaps += worker->sequence_gaps();
        errors += worker->errors();
        disconnects += worker->disconnects();
        for (std::size_t room = 0; room < options.rooms; ++room) {
            room_published[room] += worker->published()[room];
            room_delivered[room] += worker->delivered()[room];
        }
    }

    // Every acknowledged message must reach every member of its room.
    std::vector<std::uint64_t> room_members(options.rooms, 0);
    for (const BenchClient* client : clients) {
        ++room_members[client->room_index];
    }
    std::uint64_t expected = 0;
    std::uint64_t actual = 0;
    for (std::size_t room = 0; room < options.rooms; ++room) {
        expected += room_published[room] * room_members[room];
        actual += room_delivered[room];
    }
    const std::uint64_t lost = expected > actual ? expected - actual : 0;

    const double seconds = static_cast<double>(measure_end - measure_start) / 1e9;
    const double publish_rate = static_cast<double>(published) / seconds;
    const double delivery_rate = static_cast<double>(delivered) / seconds;

    if (options.json) {
        std::printf(
            "{\"clients\":%zu,\"rooms\":%zu,\"publishers\":%zu,\"target_rate\":%llu,"
            "\"body_bytes\":%zu,\"published_per_s\":%.0f,\"delivered_per_s\":%.0f,"
            "\"p50_ms\":%.3f,\"p90_ms\":%.3f,\"p99_ms\":%.3f,\"p999_ms\":%.3f,\"max_ms\":%.3f,"
            "\"lost\":%llu,\"sequence_gaps\":%llu,\"errors\":%llu,\"disconnects\":%llu}\n",
            options.clients, options.rooms, options.publishers,
            static_cast<unsigned long long>(options.rate), options.body_size, publish_rate,
            delivery_rate, to_ms(latency.percentile(50)), to_ms(latency.percentile(90)),
            to_ms(latency.percentile(99)), to_ms(latency.percentile(99.9)), to_ms(latency.max()),
            static_cast<unsigned long long>(lost), static_cast<unsigned long long>(gaps),
            static_cast<unsigned long long>(errors), static_cast<unsigned long long>(disconnects));
    } else {
        std::printf("clients %zu, rooms %zu, publishers %zu, body %zu bytes, %s\n", options.clients,
                    options.rooms, options.publishers, options.body_size,
                    options.rate > 0 ? std::format("open loop at {}/s", options.rate).c_str()
                                     : "closed loop");
        std::printf("published   %12.0f msg/s\n", publish_rate);
        std::printf("delivered   %12.0f msg/s\n", delivery_rate);
        std::printf(
            "latency     p50 %.3f ms   p90 %.3f ms   p99 %.3f ms   p99.9 %.3f ms   max "
            "%.3f ms\n",
            to_ms(latency.percentile(50)), to_ms(latency.percentile(90)),
            to_ms(latency.percentile(99)), to_ms(latency.percentile(99.9)), to_ms(latency.max()));
        std::printf("integrity   %llu lost, %llu sequence gaps, %llu errors, %llu disconnects\n",
                    static_cast<unsigned long long>(lost), static_cast<unsigned long long>(gaps),
                    static_cast<unsigned long long>(errors),
                    static_cast<unsigned long long>(disconnects));
        if (disconnects > 0) {
            std::printf(
                "note        the server disconnected clients that fell behind, so 'lost' "
                "includes what they never received\n");
        }
    }
    return lost == 0 && gaps == 0 ? 0 : 1;
}
