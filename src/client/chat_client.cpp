#include "client/chat_client.h"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <utility>

#ifdef ORBIT_HAS_TLS
#include "net/tls.h"
#endif

namespace orbit::client {
namespace {

using namespace protocol;

// A peer is presumed dead after this many heartbeats of silence.
constexpr double kMissedHeartbeatLimit = 2.5;

}  // namespace

ChatClient::ChatClient(net::EventLoop& loop, Options options, Events events)
    : m_loop(loop), m_options(std::move(options)), m_events(std::move(events)) {}

ChatClient::~ChatClient() {
    cancel_timers();
}

void ChatClient::cancel_timers() {
    for (TimerWheel::TimerId* timer : {&m_retry_timer, &m_connect_timer, &m_heartbeat_timer}) {
        if (*timer != 0) {
            m_loop.cancel_timer(*timer);
            *timer = 0;
        }
    }
}

void ChatClient::set_state(State state, const std::string& detail) {
    m_state = state;
    if (m_events.on_state) {
        m_events.on_state(state, detail);
    }
}

void ChatClient::start() {
    if (m_state == State::Idle) {
        connect();
    }
}

void ChatClient::stop() {
    cancel_timers();
    set_state(State::Stopped);
    if (m_connection) {
        m_connection->close_after_flush();
    }
}

void ChatClient::connect() {
    set_state(State::Connecting);
    try {
        m_addresses = net::resolve_tcp(m_options.host, m_options.port);
    } catch (const std::exception& error) {
        schedule_retry(error.what());
        return;
    }
    m_next_address = 0;
    connect_to_next_address();
}

void ChatClient::connect_to_next_address() {
    const net::SocketAddress& address = m_addresses[m_next_address++];
    try {
        std::unique_ptr<net::SecureChannel> channel;
#ifdef ORBIT_HAS_TLS
        if (m_options.tls) {
            channel = m_options.tls->create_channel(m_options.host);
        }
#endif
        net::Connection::Handlers handlers;
        handlers.on_connected = [this] { on_connected(); };
        handlers.on_data = [this](ByteBuffer& input) { on_data(input); };
        handlers.on_close = [this](net::CloseReason reason) { on_closed(reason); };

        m_connection = std::make_unique<net::Connection>(m_loop, net::connect_tcp(address),
                                                         net::Connection::Options{},
                                                         std::move(handlers), std::move(channel));
        m_connection->start_connecting();
    } catch (const std::exception& error) {
        m_connection.reset();
        if (m_next_address < m_addresses.size()) {
            connect_to_next_address();
        } else {
            schedule_retry(error.what());
        }
        return;
    }
    m_connect_timer = m_loop.run_after(m_options.connect_timeout_ms, [this] {
        m_connect_timer = 0;
        m_close_detail = "connection timed out";
        if (m_connection) {
            m_connection->close(net::CloseReason::ConnectFailed);
        }
    });
}

void ChatClient::on_connected() {
    send(Hello{kProtocolVersion, m_options.nick, m_options.token});
}

void ChatClient::send(const AnyMessage& message) {
    if (m_connection) {
        m_connection->send(make_frame(message));
    }
}

void ChatClient::on_data(ByteBuffer& input) {
    m_last_received_ms = m_loop.now_ms();
    while (m_connection && m_connection->is_open()) {
        const auto frame = extract_frame(input.readable(), m_max_frame_size);
        if (frame.status == ExtractStatus::NeedMore) {
            return;
        }
        if (frame.status != ExtractStatus::Complete) {
            m_close_detail = "server sent an invalid frame";
            m_connection->close();
            return;
        }
        const auto message = decode(frame.body);
        input.consume(frame.consumed);
        if (!message) {
            m_close_detail = "server sent a malformed message";
            m_connection->close();
            return;
        }
        handle(*message);
    }
}

void ChatClient::handle(const AnyMessage& message) {
    if (const auto* welcome = std::get_if<Welcome>(&message)) {
        on_welcome(*welcome);
    } else if (const auto* chat = std::get_if<Message>(&message)) {
        const auto room = m_rooms.find(chat->room);
        if (room == m_rooms.end() || chat->seq <= room->second.last_seq) {
            return;  // Seen already; a rejoin can overlap what was delivered.
        }
        room->second.last_seq = chat->seq;
        if (m_events.on_message) {
            m_events.on_message(*chat, chat->seq <= room->second.history_until);
        }
    } else if (const auto* joined = std::get_if<Joined>(&message)) {
        RoomPosition& position = m_rooms[joined->room];
        const std::uint64_t next_needed = position.last_seq + 1;
        const std::uint64_t missed = position.last_seq != 0 && joined->first_seq > next_needed
                                         ? joined->first_seq - next_needed
                                         : 0;
        position.history_until = joined->head_seq;
        if (m_events.on_joined) {
            m_events.on_joined(*joined, missed);
        }
    } else if (const auto* left = std::get_if<Left>(&message)) {
        m_rooms.erase(left->room);
        if (m_events.on_left) {
            m_events.on_left(*left);
        }
    } else if (const auto* presence = std::get_if<Presence>(&message)) {
        if (m_events.on_presence) {
            m_events.on_presence(*presence);
        }
    } else if (const auto* dm = std::get_if<DirectMessage>(&message)) {
        if (m_events.on_direct) {
            m_events.on_direct(*dm);
        }
    } else if (const auto* who_reply = std::get_if<WhoReply>(&message)) {
        if (m_events.on_who) {
            m_events.on_who(*who_reply);
        }
    } else if (const auto* rooms = std::get_if<RoomList>(&message)) {
        if (m_events.on_rooms) {
            m_events.on_rooms(*rooms);
        }
    } else if (const auto* error = std::get_if<Error>(&message)) {
        on_server_error(*error);
    }
}

void ChatClient::on_welcome(const Welcome& welcome) {
    if (m_connect_timer != 0) {
        m_loop.cancel_timer(m_connect_timer);
        m_connect_timer = 0;
    }
    m_attempt = 0;
    m_ever_online = true;
    m_heartbeat_ms = welcome.heartbeat_ms;
    m_max_frame_size = welcome.max_frame_size;
    m_last_received_ms = m_loop.now_ms();
    set_state(State::Online);

    for (const auto& [room, position] : m_rooms) {
        send(Join{room, position.last_seq});
    }
    if (m_heartbeat_ms > 0) {
        m_heartbeat_timer = m_loop.run_after(m_heartbeat_ms, [this] { heartbeat(); });
    }
}

void ChatClient::heartbeat() {
    m_heartbeat_timer = 0;
    if (!m_connection) {
        return;
    }
    const auto limit =
        static_cast<std::uint64_t>(static_cast<double>(m_heartbeat_ms) * kMissedHeartbeatLimit);
    if (m_loop.now_ms() - m_last_received_ms > limit) {
        m_close_detail = "server stopped responding";
        m_connection->close();
        return;
    }
    send(Ping{m_loop.now_ms()});
    m_heartbeat_timer = m_loop.run_after(m_heartbeat_ms, [this] { heartbeat(); });
}

void ChatClient::on_server_error(const Error& error) {
    const std::string text = error.detail.empty()
                                 ? std::string(error_name(error.code))
                                 : std::format("{}: {}", error_name(error.code), error.detail);
    // A nickname can stay taken for a moment after a reconnect, until the server
    // notices the old connection has gone, so that case alone stays retryable.
    const bool fatal = error.code == ErrorCode::AuthenticationFailed ||
                       error.code == ErrorCode::UnsupportedVersion ||
                       error.code == ErrorCode::InvalidName ||
                       (error.code == ErrorCode::NicknameTaken && !m_ever_online);
    if (fatal) {
        cancel_timers();
        set_state(State::Stopped, text);
        if (m_connection) {
            m_connection->close();
        }
        return;
    }
    if (m_state == State::Online) {
        if (m_events.on_error) {
            m_events.on_error(error);
        }
    }
    m_close_detail = text;
}

void ChatClient::on_closed(net::CloseReason reason) {
    if (m_connect_timer != 0) {
        m_loop.cancel_timer(m_connect_timer);
        m_connect_timer = 0;
    }
    if (m_heartbeat_timer != 0) {
        m_loop.cancel_timer(m_heartbeat_timer);
        m_heartbeat_timer = 0;
    }
    m_connection.reset();  // on_close is the one safe place to do this.

    if (m_state == State::Stopped) {
        return;
    }
    // "localhost" commonly resolves to ::1 first while the server listens on
    // 127.0.0.1, so one refused address says nothing about the next.
    if (reason == net::CloseReason::ConnectFailed && m_next_address < m_addresses.size()) {
        m_close_detail.clear();
        connect_to_next_address();
        return;
    }
    std::string detail = std::exchange(m_close_detail, {});
    if (detail.empty()) {
        switch (reason) {
            case net::CloseReason::ConnectFailed:
                detail = "connection refused";
                break;
            case net::CloseReason::TlsFailure:
                detail = "TLS handshake failed";
                break;
            case net::CloseReason::PeerClosed:
                detail = "server closed the connection";
                break;
            default:
                detail = "connection lost";
                break;
        }
    }
    schedule_retry(detail);
}

void ChatClient::schedule_retry(const std::string& detail) {
    if (!m_options.reconnect) {
        set_state(State::Stopped, detail);
        return;
    }
    const std::uint32_t exponent = std::min<std::uint32_t>(m_attempt++, 16);
    const std::uint64_t ceiling =
        std::min(m_options.max_backoff_ms, m_options.min_backoff_ms << exponent);
    // Full jitter: anywhere in [min, ceiling], not ceiling plus a little noise.
    std::uniform_int_distribution<std::uint64_t> pick(m_options.min_backoff_ms, ceiling);
    const std::uint64_t delay = pick(m_random);

    set_state(State::Backoff,
              std::format("{}; retrying in {:.1f}s", detail, static_cast<double>(delay) / 1000.0));
    m_retry_timer = m_loop.run_after(delay, [this] {
        m_retry_timer = 0;
        connect();
    });
}

void ChatClient::join(const std::string& room) {
    const auto [position, inserted] = m_rooms.try_emplace(room);
    if (m_state == State::Online) {
        send(Join{room, position->second.last_seq});
    }
}

void ChatClient::leave(const std::string& room) {
    if (m_state == State::Online) {
        send(Leave{room});
    } else {
        m_rooms.erase(room);
    }
}

bool ChatClient::publish(const std::string& room, const std::string& body) {
    if (m_state != State::Online) {
        return false;
    }
    send(Publish{m_next_correlation++, room, body});
    return true;
}

bool ChatClient::direct(const std::string& nick, const std::string& body) {
    if (m_state != State::Online) {
        return false;
    }
    send(Direct{m_next_correlation++, nick, body});
    return true;
}

void ChatClient::who(const std::string& room) {
    send(Who{room});
}

void ChatClient::list_rooms() {
    send(ListRooms{});
}

}  // namespace orbit::client
