#include "server/session.h"

#include <cstdint>
#include <format>
#include <utility>

#include "common/clock.h"
#include "common/log.h"
#include "server/shard.h"

namespace orbit::server {
namespace {

using namespace protocol;

// Once a client has ignored this many rate-limit errors in a row it is treated as
// abusive: every refusal costs an outbound frame, so answering forever would
// itself be a way to burn server bandwidth.
constexpr std::uint32_t kMaxConsecutiveRateLimits = 1000;

// Compares in time independent of where the inputs differ, so that response
// timing does not reveal how much of a guessed token was right.
bool constant_time_equal(std::string_view a, std::string_view b) {
    std::uint8_t difference = a.size() == b.size() ? 0 : 1;
    const std::size_t length = std::max(a.size(), b.size());
    for (std::size_t i = 0; i < length; ++i) {
        const auto left = static_cast<std::uint8_t>(i < a.size() ? a[i] : 0);
        const auto right = static_cast<std::uint8_t>(i < b.size() ? b[i] : 0);
        difference = static_cast<std::uint8_t>(difference | (left ^ right));
    }
    return difference == 0;
}

}  // namespace

Session::Session(Shard& shard, ServerContext& context, SessionId id, net::UniqueFd fd,
                 std::unique_ptr<net::SecureChannel> channel)
    : m_shard(shard),
      m_context(context),
      m_id(id),
      m_peer(net::peer_address(fd.get())),
      m_rate_limiter(context.config.rate_per_second, context.config.rate_burst) {
    net::Connection::Options options;
    options.max_queued_bytes = context.config.max_queued_bytes;
    options.linger_ms = context.config.shutdown_grace_ms / 2;

    net::Connection::Handlers handlers;
    handlers.on_data = [this](ByteBuffer& input) { on_data(input); };
    handlers.on_close = [this](net::CloseReason reason) { on_close(reason); };
    m_connection = std::make_unique<net::Connection>(m_shard.loop(), std::move(fd), options,
                                                     std::move(handlers), std::move(channel));
}

Session::~Session() {
    if (m_idle_timer != 0) {
        m_shard.loop().cancel_timer(m_idle_timer);
    }
}

void Session::start() {
    m_last_activity_ms = m_shard.loop().now_ms();
    m_connection->start();
    schedule_idle_check(m_context.config.handshake_timeout_ms);
    log::debug("session {:#x}: connected from {}", m_id, m_peer);
}

void Session::terminate(ErrorCode code, std::string detail) {
    m_connection->send(make_frame(Error{0, code, std::move(detail)}));
    m_connection->close_after_flush();
}

void Session::on_data(ByteBuffer& input) {
    m_last_activity_ms = m_shard.loop().now_ms();
    while (m_connection->is_open()) {
        const auto frame = extract_frame(input.readable(), m_context.config.max_frame_size);
        switch (frame.status) {
            case ExtractStatus::NeedMore:
                return;
            case ExtractStatus::Oversized:
                protocol_violation("frame exceeds the size limit");
                return;
            case ExtractStatus::Empty:
                protocol_violation("empty frame");
                return;
            case ExtractStatus::Complete:
                break;
        }
        auto message = decode(frame.body);
        input.consume(frame.consumed);
        if (!message) {
            protocol_violation("malformed frame");
            return;
        }
        dispatch(std::move(*message));
    }
}

void Session::dispatch(AnyMessage&& message) {
    std::visit(
        [this](auto&& typed) {
            using T = std::decay_t<decltype(typed)>;
            constexpr bool is_hello = std::is_same_v<T, Hello>;
            constexpr bool is_client_message =
                is_hello || std::is_same_v<T, Join> || std::is_same_v<T, Leave> ||
                std::is_same_v<T, Publish> || std::is_same_v<T, Direct> ||
                std::is_same_v<T, Who> || std::is_same_v<T, ListRooms> || std::is_same_v<T, Ping>;

            if constexpr (!is_client_message) {
                protocol_violation("server message sent by a client");
            } else {
                if ((m_state == State::AwaitingHello) != is_hello) {
                    protocol_violation(is_hello ? "duplicate Hello" : "expected Hello");
                    return;
                }
                handle(std::forward<decltype(typed)>(typed));
            }
        },
        std::move(message));
}

void Session::handle(Hello&& hello) {
    if (hello.version != kProtocolVersion) {
        terminate(ErrorCode::UnsupportedVersion, std::format("server speaks {}", kProtocolVersion));
        return;
    }
    if (!m_context.config.auth_token.empty() &&
        !constant_time_equal(hello.token, m_context.config.auth_token)) {
        log::warn("authentication failed from {}", m_peer);
        terminate(ErrorCode::AuthenticationFailed, {});
        return;
    }
    if (!is_valid_name(hello.nick)) {
        terminate(ErrorCode::InvalidName, "nicknames are 1-32 of A-Z a-z 0-9 _ -");
        return;
    }
    if (!m_context.nicks.try_claim(hello.nick, m_id)) {
        terminate(ErrorCode::NicknameTaken, hello.nick);
        return;
    }

    m_nick = std::move(hello.nick);
    m_state = State::Active;
    m_counted_as_session = true;
    m_context.metrics.sessions_active.add();

    const ServerConfig& config = m_context.config;
    send(make_frame(Welcome{m_id, static_cast<std::uint32_t>(config.heartbeat_ms),
                            static_cast<std::uint32_t>(config.max_frame_size), ORBIT_VERSION}));
    log::debug("session {:#x}: identified as {}", m_id, m_nick);
}

void Session::handle(Join&& join) {
    if (!is_valid_name(join.room)) {
        reply_error(ErrorCode::InvalidName, 0, "room names are 1-32 of A-Z a-z 0-9 _ -");
        return;
    }
    if (m_rooms.size() + m_pending_joins >= m_context.config.max_rooms_per_session) {
        reply_error(ErrorCode::Overloaded, 0, "too many rooms");
        return;
    }
    ++m_pending_joins;
    m_shard.join_room(m_id, m_nick, std::move(join.room), join.since_seq);
}

void Session::handle(Leave&& leave) {
    m_shard.leave_room(m_id, std::move(leave.room));
}

bool Session::admit_message(std::uint64_t correlation) {
    if (m_rate_limiter.try_acquire(m_shard.loop().now_ms())) {
        m_consecutive_rate_limits = 0;
        return true;
    }
    m_context.metrics.rate_limited.add();
    if (++m_consecutive_rate_limits >= kMaxConsecutiveRateLimits) {
        terminate(ErrorCode::RateLimited, "disconnected for flooding");
    } else {
        reply_error(ErrorCode::RateLimited, correlation);
    }
    return false;
}

bool Session::validate_body(const std::string& body, std::uint64_t correlation) {
    if (body.empty() || body.size() > m_context.config.max_body_size) {
        reply_error(ErrorCode::InvalidMessage, correlation, "body is empty or too long");
        return false;
    }
    if (!is_valid_utf8(body)) {
        reply_error(ErrorCode::InvalidMessage, correlation, "body is not valid UTF-8");
        return false;
    }
    return true;
}

void Session::handle(Publish&& publish) {
    if (!admit_message(publish.correlation) ||
        !validate_body(publish.body, publish.correlation)) {
        return;
    }
    m_shard.publish(PublishRequest{m_id, m_nick, publish.correlation, std::move(publish.room),
                                   std::move(publish.body), monotonic_ns()});
}

void Session::handle(Direct&& direct) {
    if (!admit_message(direct.correlation) || !validate_body(direct.body, direct.correlation)) {
        return;
    }
    const auto target = m_context.nicks.find(direct.to);
    if (!target) {
        reply_error(ErrorCode::UnknownUser, direct.correlation, direct.to);
        return;
    }
    const std::uint64_t now = wall_clock_ms();
    m_shard.send_to(*target,
                    make_frame(DirectMessage{m_nick, now, std::move(direct.body)}));
    send(make_frame(Ack{direct.correlation, 0, now}));
    m_context.metrics.direct_messages.add();
}

void Session::handle(Who&& who) {
    m_shard.who(m_id, std::move(who.room));
}

void Session::handle(ListRooms&&) {
    send(make_frame(RoomList{m_context.directory.snapshot()}));
}

void Session::handle(Ping&& ping) {
    send(make_frame(Pong{ping.nonce}));
}

void Session::reply_error(ErrorCode code, std::uint64_t correlation, std::string detail) {
    send(make_frame(Error{correlation, code, std::move(detail)}));
}

void Session::protocol_violation(std::string detail) {
    m_context.metrics.protocol_errors.add();
    log::debug("session {:#x}: protocol violation: {}", m_id, detail);
    terminate(ErrorCode::ProtocolViolation, std::move(detail));
}

void Session::schedule_idle_check(std::uint64_t delay_ms) {
    m_idle_timer = m_shard.loop().run_after(delay_ms, [this] {
        m_idle_timer = 0;
        on_idle_check();
    });
}

// Activity only updates a timestamp, so traffic costs nothing here. The timer
// wakes at the earliest moment the session could have expired, and goes back to
// sleep for the remainder if it turns out not to have.
void Session::on_idle_check() {
    const std::uint64_t limit = m_state == State::AwaitingHello
                                    ? m_context.config.handshake_timeout_ms
                                    : m_context.config.idle_timeout_ms;
    const std::uint64_t idle = m_shard.loop().now_ms() - m_last_activity_ms;
    if (idle >= limit) {
        terminate(ErrorCode::TimedOut, m_state == State::AwaitingHello ? "no Hello received"
                                                                       : "idle timeout");
        return;
    }
    schedule_idle_check(limit - idle);
}

void Session::on_close(net::CloseReason reason) {
    ServerMetrics& metrics = m_context.metrics;
    switch (reason) {
        case net::CloseReason::PeerClosed:
            metrics.closed_peer.add();
            break;
        case net::CloseReason::IoError:
        case net::CloseReason::ConnectFailed:
            metrics.closed_error.add();
            break;
        case net::CloseReason::Local:
            metrics.closed_local.add();
            break;
        case net::CloseReason::SlowConsumer:
            metrics.closed_slow.add();
            log::warn("session {:#x} ({}): disconnected as a slow consumer", m_id, m_nick);
            break;
        case net::CloseReason::TlsFailure:
            metrics.closed_tls.add();
            break;
    }
    if (m_counted_as_session) {
        metrics.sessions_active.sub();
        m_context.nicks.release(m_nick, m_id);
    }
    m_shard.session_closed(m_id);  // Destroys this object.
}

}  // namespace orbit::server
