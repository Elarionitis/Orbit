#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Orbit wire protocol, version 1. See docs/PROTOCOL.md for the specification.
//
// Every frame is a 4-byte big-endian length followed by that many bytes, of
// which the first is the message type. Fields are big-endian; strings are
// length-prefixed. The length excludes itself.
namespace orbit::protocol {

inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr std::size_t kLengthPrefixSize = 4;
inline constexpr std::size_t kDefaultMaxFrameSize = 64 * 1024;
inline constexpr std::size_t kMaxNameLength = 32;

enum class MessageType : std::uint8_t {
    // Client to server.
    Hello = 0x01,
    Join = 0x02,
    Leave = 0x03,
    Publish = 0x04,
    Direct = 0x05,
    Who = 0x06,
    ListRooms = 0x07,
    Ping = 0x08,
    // Server to client.
    Welcome = 0x81,
    Error = 0x82,
    Joined = 0x83,
    Left = 0x84,
    Message = 0x85,
    Presence = 0x86,
    DirectMessage = 0x87,
    Ack = 0x88,
    WhoReply = 0x89,
    RoomList = 0x8A,
    Pong = 0x8B,
};

enum class ErrorCode : std::uint16_t {
    ProtocolViolation = 1,
    UnsupportedVersion = 2,
    AuthenticationFailed = 3,
    NicknameTaken = 4,
    InvalidName = 5,
    InvalidMessage = 6,
    NotInRoom = 7,
    UnknownUser = 8,
    RateLimited = 9,
    Overloaded = 10,
    ServerFull = 11,
    ServerShutdown = 12,
    StorageFailure = 13,
    TimedOut = 14,
};

struct Hello {
    std::uint16_t version = kProtocolVersion;
    std::string nick;
    std::string token;
    bool operator==(const Hello&) const = default;
};

// `since_seq` is the last sequence number the client has seen in this room, or
// zero for "send recent history". The server replays everything newer.
struct Join {
    std::string room;
    std::uint64_t since_seq = 0;
    bool operator==(const Join&) const = default;
};

struct Leave {
    std::string room;
    bool operator==(const Leave&) const = default;
};

// `correlation` is chosen by the client and echoed in the Ack or Error.
struct Publish {
    std::uint64_t correlation = 0;
    std::string room;
    std::string body;
    bool operator==(const Publish&) const = default;
};

struct Direct {
    std::uint64_t correlation = 0;
    std::string to;
    std::string body;
    bool operator==(const Direct&) const = default;
};

struct Who {
    std::string room;
    bool operator==(const Who&) const = default;
};

struct ListRooms {
    bool operator==(const ListRooms&) const = default;
};

struct Ping {
    std::uint64_t nonce = 0;
    bool operator==(const Ping&) const = default;
};

struct Welcome {
    std::uint64_t session_id = 0;
    std::uint32_t heartbeat_ms = 0;
    std::uint32_t max_frame_size = 0;
    std::string server_version;
    bool operator==(const Welcome&) const = default;
};

struct Error {
    std::uint64_t correlation = 0;
    ErrorCode code = ErrorCode::ProtocolViolation;
    std::string detail;
    bool operator==(const Error&) const = default;
};

// `head_seq` is the newest message in the room at the instant of joining: any
// Message for this room with seq <= head_seq is replayed history, and everything
// after is live. `first_seq` is the oldest message the server can still replay.
struct Joined {
    std::string room;
    std::uint64_t head_seq = 0;
    std::uint64_t first_seq = 0;
    std::uint32_t member_count = 0;
    bool operator==(const Joined&) const = default;
};

struct Left {
    std::string room;
    bool operator==(const Left&) const = default;
};

struct Message {
    std::string room;
    std::uint64_t seq = 0;
    std::uint64_t timestamp_ms = 0;
    std::string nick;
    std::string body;
    bool operator==(const Message&) const = default;
};

struct Presence {
    std::string room;
    std::string nick;
    bool joined = false;
    std::uint64_t timestamp_ms = 0;
    bool operator==(const Presence&) const = default;
};

struct DirectMessage {
    std::string from;
    std::uint64_t timestamp_ms = 0;
    std::string body;
    bool operator==(const DirectMessage&) const = default;
};

struct Ack {
    std::uint64_t correlation = 0;
    std::uint64_t seq = 0;
    std::uint64_t timestamp_ms = 0;
    bool operator==(const Ack&) const = default;
};

struct WhoReply {
    std::string room;
    std::vector<std::string> nicks;
    bool operator==(const WhoReply&) const = default;
};

struct RoomInfo {
    std::string name;
    std::uint32_t member_count = 0;
    bool operator==(const RoomInfo&) const = default;
};

struct RoomList {
    std::vector<RoomInfo> rooms;
    bool operator==(const RoomList&) const = default;
};

struct Pong {
    std::uint64_t nonce = 0;
    bool operator==(const Pong&) const = default;
};

using AnyMessage =
    std::variant<Hello, Join, Leave, Publish, Direct, Who, ListRooms, Ping, Welcome, Error, Joined,
                 Left, Message, Presence, DirectMessage, Ack, WhoReply, RoomList, Pong>;

// An immutable encoded frame. Fan-out hands the same frame to every recipient,
// so a message is serialised once no matter how many members a room has.
using Frame = std::shared_ptr<const std::vector<std::uint8_t>>;

std::vector<std::uint8_t> encode(const AnyMessage& message);

inline Frame make_frame(const AnyMessage& message) {
    return std::make_shared<const std::vector<std::uint8_t>>(encode(message));
}

// Decodes a frame body (type byte plus payload, without the length prefix).
// Returns nullopt for unknown types, truncated fields, or trailing bytes.
std::optional<AnyMessage> decode(std::span<const std::uint8_t> frame_body);

enum class ExtractStatus {
    Complete,   // A full frame is available.
    NeedMore,   // Wait for more bytes.
    Oversized,  // Declared length exceeds the limit; the stream cannot be trusted.
    Empty,      // Declared length is zero; there is no type byte.
};

struct Extracted {
    ExtractStatus status = ExtractStatus::NeedMore;
    std::span<const std::uint8_t> body;  // Valid when status is Complete.
    std::size_t consumed = 0;            // Bytes to drop from the input.
};

// Finds the first frame in a stream buffer without copying it. Handles arbitrary
// fragmentation: the caller simply retries when more bytes arrive.
Extracted extract_frame(std::span<const std::uint8_t> input, std::size_t max_frame_size);

// Room names and nicknames: 1-32 of [A-Za-z0-9_-].
bool is_valid_name(std::string_view name);

// Well-formed UTF-8, rejecting overlong forms, surrogates and code points past U+10FFFF.
bool is_valid_utf8(std::string_view text);

std::string_view error_name(ErrorCode code);

}  // namespace orbit::protocol
