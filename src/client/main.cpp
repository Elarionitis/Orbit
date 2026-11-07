#include <signal.h>
#include <unistd.h>

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <format>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "client/terminal_app.h"
#include "common/log.h"
#include "net/event_loop.h"
#include "protocol/messages.h"
#ifdef ORBIT_HAS_TLS
#include "net/tls.h"
#endif

namespace {

constexpr std::string_view kUsage = R"(Usage: orbit_client [options]

  --host HOST         Server address (default 127.0.0.1)
  --port PORT         Server port (default 8080)
  --nick NAME         Nickname (default: your login name)
  --room NAME         Room to join on connect (default general)
  --token-file FILE   Authentication token, if the server needs one
  --tls               Connect with TLS, verifying against the system trust store
  --ca FILE           Verify the server against this CA certificate (implies --tls)
  --insecure          Skip certificate verification (implies --tls; development only)
  --no-reconnect      Exit when the connection is lost
  --help              Show this help
  --version           Show the version
)";

std::string default_nick() {
    const char* user = std::getenv("USER");
    return user != nullptr && orbit::protocol::is_valid_name(user) ? user : "guest";
}

std::uint16_t parse_port(std::string_view text) {
    std::uint16_t port = 0;
    const auto* end = text.data() + text.size();
    const auto [parsed, error] = std::from_chars(text.data(), end, port);
    if (error != std::errc{} || parsed != end) {
        throw std::invalid_argument(std::format("invalid port '{}'", text));
    }
    return port;
}

std::string read_first_line(const std::string& path) {
    std::ifstream file(path);
    std::string line;
    if (!file || !std::getline(file, line)) {
        throw std::invalid_argument(std::format("cannot read '{}'", path));
    }
    return line;
}

}  // namespace

int main(int argc, char* argv[]) {
    using namespace orbit;

    ::signal(SIGPIPE, SIG_IGN);
    log::set_level(log::Level::Off);

    client::ChatClient::Options options;
    options.nick = default_nick();
    std::string room = "general";
    bool use_tls = false;
    bool verify = true;
    std::string ca_path;

    try {
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            const auto value = [&]() -> std::string {
                if (i + 1 >= argc) {
                    throw std::invalid_argument(std::format("{} needs a value", arg));
                }
                return argv[++i];
            };
            if (arg == "--help" || arg == "-h") {
                std::fputs(std::string(kUsage).c_str(), stdout);
                return 0;
            }
            if (arg == "--version") {
                std::printf("orbit_client %s\n", ORBIT_VERSION);
                return 0;
            }
            if (arg == "--host") {
                options.host = value();
            } else if (arg == "--port") {
                options.port = parse_port(value());
            } else if (arg == "--nick") {
                options.nick = value();
            } else if (arg == "--room") {
                room = value();
            } else if (arg == "--token-file") {
                options.token = read_first_line(value());
            } else if (arg == "--tls") {
                use_tls = true;
            } else if (arg == "--ca") {
                ca_path = value();
                use_tls = true;
            } else if (arg == "--insecure") {
                verify = false;
                use_tls = true;
            } else if (arg == "--no-reconnect") {
                options.reconnect = false;
            } else {
                throw std::invalid_argument(std::format("unknown option '{}'", arg));
            }
        }
        if (!protocol::is_valid_name(options.nick)) {
            throw std::invalid_argument("nicknames are 1-32 of A-Z a-z 0-9 _ -");
        }
        if (use_tls) {
#ifdef ORBIT_HAS_TLS
            options.tls = net::TlsContext::client(ca_path, verify);
#else
            throw std::invalid_argument("this build has no TLS support");
#endif
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "orbit_client: %s\nTry --help.\n", error.what());
        return 2;
    }

    net::EventLoop loop;
    client::TerminalApp app(loop, std::move(options), room);
    loop.post([&app] { app.start(); });
    loop.run();
    return app.failed() ? 1 : 0;
}
