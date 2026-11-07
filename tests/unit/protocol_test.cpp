#include <gtest/gtest.h>

#include <random>
#include <string>
#include <vector>

#include "common/byte_buffer.h"
#include "protocol/messages.h"

using namespace orbit::protocol;

namespace {

std::vector<AnyMessage> sample_messages() {
    return {
        Hello{1, "ada", "secret"},
        Join{"general", 42},
        Leave{"general"},
        Publish{7, "general", "hello \xF0\x9F\x9A\x80"},
        Direct{8, "grace", "psst"},
        Who{"general"},
        ListRooms{},
        Ping{99},
        Welcome{123, 15000, 65536, "2.0.0"},
        Error{7, ErrorCode::RateLimited, "slow down"},
        Joined{"general", 100, 50, 3},
        Left{"general"},
        Message{"general", 101, 1700000000000, "ada", "body"},
        Presence{"general", "ada", true, 1700000000000},
        DirectMessage{"ada", 1700000000000, "psst"},
        Ack{7, 101, 1700000000000},
        WhoReply{"general", {"ada", "grace"}},
        RoomList{{{"general", 3}, {"random", 1}}},
        Pong{99},
    };
}

std::span<const std::uint8_t> body_of(const std::vector<std::uint8_t>& frame) {
    return std::span<const std::uint8_t>(frame).subspan(kLengthPrefixSize);
}

}  // namespace

TEST(Codec, EveryMessageRoundTrips) {
    for (const auto& message : sample_messages()) {
        const auto frame = encode(message);
        const auto decoded = decode(body_of(frame));
        ASSERT_TRUE(decoded.has_value()) << "type index " << message.index();
        EXPECT_EQ(*decoded, message) << "type index " << message.index();
    }
}

TEST(Codec, LengthPrefixCountsEverythingAfterItself) {
    const auto frame = encode(Ping{1});
    ASSERT_EQ(frame.size(), 4U + 1U + 8U);
    EXPECT_EQ(frame[3], 9);
}

TEST(Codec, IntegersAreBigEndian) {
    const auto frame = encode(Ping{0x0102030405060708ULL});
    EXPECT_EQ(frame[4], static_cast<std::uint8_t>(MessageType::Ping));
    EXPECT_EQ(frame[5], 0x01);
    EXPECT_EQ(frame[12], 0x08);
}

TEST(Codec, RejectsEveryTruncation) {
    for (const auto& message : sample_messages()) {
        const auto frame = encode(message);
        const auto body = body_of(frame);
        for (std::size_t length = 0; length < body.size(); ++length) {
            EXPECT_FALSE(decode(body.first(length)).has_value())
                << "type index " << message.index() << " truncated to " << length;
        }
    }
}

TEST(Codec, RejectsTrailingBytes) {
    auto frame = encode(Ping{1});
    frame.push_back(0);
    EXPECT_FALSE(decode(body_of(frame)).has_value());
}

TEST(Codec, RejectsUnknownType) {
    const std::vector<std::uint8_t> body{0x7F};
    EXPECT_FALSE(decode(body).has_value());
}

TEST(Codec, RejectsStringLengthPastEndOfFrame) {
    // Leave with a declared room length of 0xFFFF but no bytes behind it.
    const std::vector<std::uint8_t> body{static_cast<std::uint8_t>(MessageType::Leave), 0xFF, 0xFF};
    EXPECT_FALSE(decode(body).has_value());
}

TEST(Codec, RejectsHugeListCountWithoutAllocating) {
    std::vector<std::uint8_t> body{static_cast<std::uint8_t>(MessageType::RoomList), 0xFF, 0xFF,
                                   0xFF, 0xFF};
    EXPECT_FALSE(decode(body).has_value());
}

TEST(Codec, SurvivesRandomGarbage) {
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> byte(0, 255);
    std::uniform_int_distribution<std::size_t> size(0, 256);
    for (int round = 0; round < 50'000; ++round) {
        std::vector<std::uint8_t> body(size(rng));
        for (auto& b : body) {
            b = static_cast<std::uint8_t>(byte(rng));
        }
        if (const auto decoded = decode(body)) {
            // Anything accepted must re-encode to exactly the same bytes.
            const auto frame = encode(*decoded);
            EXPECT_TRUE(
                std::equal(body.begin(), body.end(), body_of(frame).begin(), body_of(frame).end()));
        }
    }
}

TEST(ExtractFrame, NeedsMoreForEveryPartialPrefix) {
    const auto frame = encode(Publish{1, "room", "some body text"});
    for (std::size_t length = 0; length < frame.size(); ++length) {
        const auto result = extract_frame(std::span(frame).first(length), kDefaultMaxFrameSize);
        EXPECT_EQ(result.status, ExtractStatus::NeedMore) << length;
    }
    EXPECT_EQ(extract_frame(frame, kDefaultMaxFrameSize).status, ExtractStatus::Complete);
}

TEST(ExtractFrame, FlagsOversizedFrameFromHeaderAlone) {
    const std::vector<std::uint8_t> header{0x7F, 0xFF, 0xFF, 0xFF};
    EXPECT_EQ(extract_frame(header, 1024).status, ExtractStatus::Oversized);
}

TEST(ExtractFrame, AcceptsFrameExactlyAtTheLimit) {
    const auto frame = encode(Ping{1});
    EXPECT_EQ(extract_frame(frame, 9).status, ExtractStatus::Complete);
    EXPECT_EQ(extract_frame(frame, 8).status, ExtractStatus::Oversized);
}

TEST(ExtractFrame, FlagsZeroLengthFrame) {
    const std::vector<std::uint8_t> header{0, 0, 0, 0};
    EXPECT_EQ(extract_frame(header, 1024).status, ExtractStatus::Empty);
}

// TCP has no message boundaries. Whatever chunk sizes the network delivers, the
// decoder must produce the same messages in the same order.
TEST(ExtractFrame, ReassemblesStreamAtAnyFragmentation) {
    const auto messages = sample_messages();
    std::vector<std::uint8_t> stream;
    for (const auto& message : messages) {
        const auto frame = encode(message);
        stream.insert(stream.end(), frame.begin(), frame.end());
    }

    for (std::size_t chunk = 1; chunk <= 64; ++chunk) {
        orbit::ByteBuffer buffer;
        std::vector<AnyMessage> received;
        for (std::size_t offset = 0; offset < stream.size(); offset += chunk) {
            const std::size_t count = std::min(chunk, stream.size() - offset);
            buffer.append(std::span(stream).subspan(offset, count));
            for (;;) {
                const auto result = extract_frame(buffer.readable(), kDefaultMaxFrameSize);
                if (result.status != ExtractStatus::Complete) {
                    break;
                }
                auto decoded = decode(result.body);
                ASSERT_TRUE(decoded.has_value());
                received.push_back(std::move(*decoded));
                buffer.consume(result.consumed);
            }
        }
        EXPECT_EQ(received, messages) << "chunk size " << chunk;
    }
}

TEST(Names, AcceptsAllowedCharacters) {
    EXPECT_TRUE(is_valid_name("a"));
    EXPECT_TRUE(is_valid_name("Ada_Lovelace-1815"));
    EXPECT_TRUE(is_valid_name(std::string(32, 'x')));
}

TEST(Names, RejectsEverythingElse) {
    EXPECT_FALSE(is_valid_name(""));
    EXPECT_FALSE(is_valid_name(std::string(33, 'x')));
    EXPECT_FALSE(is_valid_name("has space"));
    EXPECT_FALSE(is_valid_name("new\nline"));
    EXPECT_FALSE(is_valid_name("esc\x1b[31m"));
    EXPECT_FALSE(is_valid_name("caf\xC3\xA9"));
}

TEST(Utf8, AcceptsWellFormedText) {
    EXPECT_TRUE(is_valid_utf8(""));
    EXPECT_TRUE(is_valid_utf8("plain ascii"));
    EXPECT_TRUE(is_valid_utf8("caf\xC3\xA9"));
    EXPECT_TRUE(is_valid_utf8("\xE2\x82\xAC"));
    EXPECT_TRUE(is_valid_utf8("\xF0\x9F\x9A\x80"));
    EXPECT_TRUE(is_valid_utf8("\xF4\x8F\xBF\xBF"));  // U+10FFFF
}

TEST(Utf8, RejectsMalformedSequences) {
    EXPECT_FALSE(is_valid_utf8("\x80"));              // Stray continuation.
    EXPECT_FALSE(is_valid_utf8("\xC0\xAF"));          // Overlong.
    EXPECT_FALSE(is_valid_utf8("\xE0\x80\xAF"));      // Overlong.
    EXPECT_FALSE(is_valid_utf8("\xF0\x80\x80\xAF"));  // Overlong.
    EXPECT_FALSE(is_valid_utf8("\xED\xA0\x80"));      // Surrogate.
    EXPECT_FALSE(is_valid_utf8("\xF4\x90\x80\x80"));  // Past U+10FFFF.
    EXPECT_FALSE(is_valid_utf8("\xC3"));              // Truncated.
    EXPECT_FALSE(is_valid_utf8("\xE2\x82"));          // Truncated.
    EXPECT_FALSE(is_valid_utf8("\xFF"));
}
