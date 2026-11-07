#include "protocol/messages.h"

#include <algorithm>
#include <type_traits>

#include "protocol/wire.h"

namespace orbit::protocol {
namespace {

// Upper bound on list lengths a peer may declare, checked before reserving any
// memory. The reader's bounds checks still apply to each element.
constexpr std::uint32_t kMaxListEntries = 100'000;

void write_body(Writer& out, const Hello& m) {
    out.u16(m.version);
    out.str(m.nick);
    out.str(m.token);
}
void write_body(Writer& out, const Join& m) {
    out.str(m.room);
    out.u64(m.since_seq);
}
void write_body(Writer& out, const Leave& m) {
    out.str(m.room);
}
void write_body(Writer& out, const Publish& m) {
    out.u64(m.correlation);
    out.str(m.room);
    out.blob(m.body);
}
void write_body(Writer& out, const Direct& m) {
    out.u64(m.correlation);
    out.str(m.to);
    out.blob(m.body);
}
void write_body(Writer& out, const Who& m) {
    out.str(m.room);
}
void write_body(Writer&, const ListRooms&) {}
void write_body(Writer& out, const Ping& m) {
    out.u64(m.nonce);
}
void write_body(Writer& out, const Welcome& m) {
    out.u64(m.session_id);
    out.u32(m.heartbeat_ms);
    out.u32(m.max_frame_size);
    out.str(m.server_version);
}
void write_body(Writer& out, const Error& m) {
    out.u64(m.correlation);
    out.u16(static_cast<std::uint16_t>(m.code));
    out.str(m.detail);
}
void write_body(Writer& out, const Joined& m) {
    out.str(m.room);
    out.u64(m.head_seq);
    out.u64(m.first_seq);
    out.u32(m.member_count);
}
void write_body(Writer& out, const Left& m) {
    out.str(m.room);
}
void write_body(Writer& out, const Message& m) {
    out.str(m.room);
    out.u64(m.seq);
    out.u64(m.timestamp_ms);
    out.str(m.nick);
    out.blob(m.body);
}
void write_body(Writer& out, const Presence& m) {
    out.str(m.room);
    out.str(m.nick);
    out.u8(m.joined ? 1 : 0);
    out.u64(m.timestamp_ms);
}
void write_body(Writer& out, const DirectMessage& m) {
    out.str(m.from);
    out.u64(m.timestamp_ms);
    out.blob(m.body);
}
void write_body(Writer& out, const Ack& m) {
    out.u64(m.correlation);
    out.u64(m.seq);
    out.u64(m.timestamp_ms);
}
void write_body(Writer& out, const WhoReply& m) {
    out.str(m.room);
    out.u32(static_cast<std::uint32_t>(m.nicks.size()));
    for (const auto& nick : m.nicks) {
        out.str(nick);
    }
}
void write_body(Writer& out, const RoomList& m) {
    out.u32(static_cast<std::uint32_t>(m.rooms.size()));
    for (const auto& room : m.rooms) {
        out.str(room.name);
        out.u32(room.member_count);
    }
}
void write_body(Writer& out, const Pong& m) {
    out.u64(m.nonce);
}

template <typename T>
constexpr MessageType type_of() {
    if constexpr (std::is_same_v<T, Hello>)
        return MessageType::Hello;
    if constexpr (std::is_same_v<T, Join>)
        return MessageType::Join;
    if constexpr (std::is_same_v<T, Leave>)
        return MessageType::Leave;
    if constexpr (std::is_same_v<T, Publish>)
        return MessageType::Publish;
    if constexpr (std::is_same_v<T, Direct>)
        return MessageType::Direct;
    if constexpr (std::is_same_v<T, Who>)
        return MessageType::Who;
    if constexpr (std::is_same_v<T, ListRooms>)
        return MessageType::ListRooms;
    if constexpr (std::is_same_v<T, Ping>)
        return MessageType::Ping;
    if constexpr (std::is_same_v<T, Welcome>)
        return MessageType::Welcome;
    if constexpr (std::is_same_v<T, Error>)
        return MessageType::Error;
    if constexpr (std::is_same_v<T, Joined>)
        return MessageType::Joined;
    if constexpr (std::is_same_v<T, Left>)
        return MessageType::Left;
    if constexpr (std::is_same_v<T, Message>)
        return MessageType::Message;
    if constexpr (std::is_same_v<T, Presence>)
        return MessageType::Presence;
    if constexpr (std::is_same_v<T, DirectMessage>)
        return MessageType::DirectMessage;
    if constexpr (std::is_same_v<T, Ack>)
        return MessageType::Ack;
    if constexpr (std::is_same_v<T, WhoReply>)
        return MessageType::WhoReply;
    if constexpr (std::is_same_v<T, RoomList>)
        return MessageType::RoomList;
    if constexpr (std::is_same_v<T, Pong>)
        return MessageType::Pong;
}

AnyMessage read_message(MessageType type, Reader& in, bool& known) {
    switch (type) {
        case MessageType::Hello: {
            Hello m;
            m.version = in.u16();
            m.nick = in.str();
            m.token = in.str();
            return m;
        }
        case MessageType::Join: {
            Join m;
            m.room = in.str();
            m.since_seq = in.u64();
            return m;
        }
        case MessageType::Leave:
            return Leave{in.str()};
        case MessageType::Publish: {
            Publish m;
            m.correlation = in.u64();
            m.room = in.str();
            m.body = in.blob();
            return m;
        }
        case MessageType::Direct: {
            Direct m;
            m.correlation = in.u64();
            m.to = in.str();
            m.body = in.blob();
            return m;
        }
        case MessageType::Who:
            return Who{in.str()};
        case MessageType::ListRooms:
            return ListRooms{};
        case MessageType::Ping:
            return Ping{in.u64()};
        case MessageType::Welcome: {
            Welcome m;
            m.session_id = in.u64();
            m.heartbeat_ms = in.u32();
            m.max_frame_size = in.u32();
            m.server_version = in.str();
            return m;
        }
        case MessageType::Error: {
            Error m;
            m.correlation = in.u64();
            m.code = static_cast<ErrorCode>(in.u16());
            m.detail = in.str();
            return m;
        }
        case MessageType::Joined: {
            Joined m;
            m.room = in.str();
            m.head_seq = in.u64();
            m.first_seq = in.u64();
            m.member_count = in.u32();
            return m;
        }
        case MessageType::Left:
            return Left{in.str()};
        case MessageType::Message: {
            Message m;
            m.room = in.str();
            m.seq = in.u64();
            m.timestamp_ms = in.u64();
            m.nick = in.str();
            m.body = in.blob();
            return m;
        }
        case MessageType::Presence: {
            Presence m;
            m.room = in.str();
            m.nick = in.str();
            m.joined = in.u8() != 0;
            m.timestamp_ms = in.u64();
            return m;
        }
        case MessageType::DirectMessage: {
            DirectMessage m;
            m.from = in.str();
            m.timestamp_ms = in.u64();
            m.body = in.blob();
            return m;
        }
        case MessageType::Ack: {
            Ack m;
            m.correlation = in.u64();
            m.seq = in.u64();
            m.timestamp_ms = in.u64();
            return m;
        }
        case MessageType::WhoReply: {
            WhoReply m;
            m.room = in.str();
            const std::uint32_t count = in.u32();
            if (count > kMaxListEntries) {
                known = false;
                return m;
            }
            for (std::uint32_t i = 0; i < count && in.ok(); ++i) {
                m.nicks.push_back(in.str());
            }
            return m;
        }
        case MessageType::RoomList: {
            RoomList m;
            const std::uint32_t count = in.u32();
            if (count > kMaxListEntries) {
                known = false;
                return m;
            }
            for (std::uint32_t i = 0; i < count && in.ok(); ++i) {
                RoomInfo room;
                room.name = in.str();
                room.member_count = in.u32();
                m.rooms.push_back(std::move(room));
            }
            return m;
        }
        case MessageType::Pong:
            return Pong{in.u64()};
    }
    known = false;
    return Ping{};
}

}  // namespace

std::vector<std::uint8_t> encode(const AnyMessage& message) {
    std::vector<std::uint8_t> frame(kLengthPrefixSize);
    Writer out(frame);
    std::visit(
        [&out](const auto& typed) {
            out.u8(static_cast<std::uint8_t>(type_of<std::decay_t<decltype(typed)>>()));
            write_body(out, typed);
        },
        message);

    const auto length = static_cast<std::uint32_t>(frame.size() - kLengthPrefixSize);
    frame[0] = static_cast<std::uint8_t>(length >> 24);
    frame[1] = static_cast<std::uint8_t>(length >> 16);
    frame[2] = static_cast<std::uint8_t>(length >> 8);
    frame[3] = static_cast<std::uint8_t>(length);
    return frame;
}

std::optional<AnyMessage> decode(std::span<const std::uint8_t> frame_body) {
    if (frame_body.empty()) {
        return std::nullopt;
    }
    Reader in(frame_body.subspan(1));
    bool known = true;
    AnyMessage message = read_message(static_cast<MessageType>(frame_body[0]), in, known);
    if (!known || !in.ok() || !in.at_end()) {
        return std::nullopt;
    }
    return message;
}

Extracted extract_frame(std::span<const std::uint8_t> input, std::size_t max_frame_size) {
    if (input.size() < kLengthPrefixSize) {
        return {};
    }
    const std::size_t length =
        static_cast<std::size_t>(input[0]) << 24 | static_cast<std::size_t>(input[1]) << 16 |
        static_cast<std::size_t>(input[2]) << 8 | static_cast<std::size_t>(input[3]);
    if (length == 0) {
        return {ExtractStatus::Empty, {}, 0};
    }
    if (length > max_frame_size) {
        return {ExtractStatus::Oversized, {}, 0};
    }
    if (input.size() < kLengthPrefixSize + length) {
        return {};
    }
    return {ExtractStatus::Complete, input.subspan(kLengthPrefixSize, length),
            kLengthPrefixSize + length};
}

bool is_valid_name(std::string_view name) {
    if (name.empty() || name.size() > kMaxNameLength) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
    });
}

bool is_valid_utf8(std::string_view text) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(text.data());
    const std::size_t size = text.size();
    std::size_t i = 0;
    while (i < size) {
        const std::uint8_t lead = bytes[i];
        std::size_t extra = 0;
        std::uint8_t low = 0x80;
        std::uint8_t high = 0xBF;
        if (lead < 0x80) {
            ++i;
            continue;
        }
        if (lead >= 0xC2 && lead <= 0xDF) {
            extra = 1;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            extra = 2;
            if (lead == 0xE0)
                low = 0xA0;  // Overlong.
            if (lead == 0xED)
                high = 0x9F;  // Surrogates.
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            extra = 3;
            if (lead == 0xF0)
                low = 0x90;  // Overlong.
            if (lead == 0xF4)
                high = 0x8F;  // Past U+10FFFF.
        } else {
            return false;
        }
        if (size - i <= extra) {
            return false;
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            const std::uint8_t next = bytes[i + k];
            if (next < low || next > high) {
                return false;
            }
            low = 0x80;
            high = 0xBF;
        }
        i += extra + 1;
    }
    return true;
}

std::string_view error_name(ErrorCode code) {
    switch (code) {
        case ErrorCode::ProtocolViolation:
            return "protocol violation";
        case ErrorCode::UnsupportedVersion:
            return "unsupported protocol version";
        case ErrorCode::AuthenticationFailed:
            return "authentication failed";
        case ErrorCode::NicknameTaken:
            return "nickname already in use";
        case ErrorCode::InvalidName:
            return "invalid name";
        case ErrorCode::InvalidMessage:
            return "invalid message";
        case ErrorCode::NotInRoom:
            return "not in room";
        case ErrorCode::UnknownUser:
            return "unknown user";
        case ErrorCode::RateLimited:
            return "rate limited";
        case ErrorCode::Overloaded:
            return "server overloaded";
        case ErrorCode::ServerFull:
            return "server full";
        case ErrorCode::ServerShutdown:
            return "server shutting down";
        case ErrorCode::StorageFailure:
            return "storage failure";
        case ErrorCode::TimedOut:
            return "timed out";
    }
    return "unknown error";
}

}  // namespace orbit::protocol
