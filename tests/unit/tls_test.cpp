#include "net/tls.h"

#include <gtest/gtest.h>

#include <string>

#include "support/temp_dir.h"
#include "support/test_certificate.h"

using namespace orbit::net;
using orbit::ByteBuffer;

namespace {

// Moves ciphertext back and forth between two channels until both go quiet, with
// no sockets involved: the benefit of keeping the TLS layer sans-IO.
struct ChannelPair {
    std::unique_ptr<SecureChannel> client;
    std::unique_ptr<SecureChannel> server;
    ByteBuffer client_plain;
    ByteBuffer server_plain;
    bool failed = false;

    void pump() {
        for (int round = 0; round < 16 && !failed; ++round) {
            std::vector<std::uint8_t> bytes;
            client->take_output(bytes);
            if (!bytes.empty() && !server->receive(bytes, server_plain)) {
                failed = true;
            }
            bytes.clear();
            server->take_output(bytes);
            if (!bytes.empty() && !client->receive(bytes, client_plain)) {
                failed = true;
            }
        }
    }
};

std::span<const std::uint8_t> bytes_of(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

std::string text_of(const ByteBuffer& buffer) {
    const auto bytes = buffer.readable();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

class TlsTest : public ::testing::Test {
protected:
    orbit::testing::TempDir dir;
    orbit::testing::TestCertificate cert{dir.path()};
    std::shared_ptr<TlsContext> server_context =
        TlsContext::server(cert.cert_path.string(), cert.key_path.string());

    ChannelPair connect(const std::shared_ptr<TlsContext>& client_context,
                        const std::string& hostname) {
        ChannelPair pair{client_context->create_channel(hostname), server_context->create_channel(),
                         {}, {}, false};
        EXPECT_TRUE(pair.server->start());
        EXPECT_TRUE(pair.client->start());
        pair.pump();
        return pair;
    }
};

}  // namespace

TEST_F(TlsTest, EncryptsInBothDirections) {
    auto pair = connect(TlsContext::client(cert.cert_path.string(), true), "localhost");
    ASSERT_FALSE(pair.failed);

    ASSERT_TRUE(pair.client->send(bytes_of("from client")));
    ASSERT_TRUE(pair.server->send(bytes_of("from server")));
    pair.pump();

    EXPECT_EQ(text_of(pair.server_plain), "from client");
    EXPECT_EQ(text_of(pair.client_plain), "from server");
}

TEST_F(TlsTest, CiphertextDoesNotContainThePlaintext) {
    auto pair = connect(TlsContext::client(cert.cert_path.string(), true), "localhost");
    ASSERT_TRUE(pair.client->send(bytes_of("attack at dawn")));

    std::vector<std::uint8_t> wire;
    pair.client->take_output(wire);
    ASSERT_FALSE(wire.empty());
    EXPECT_EQ(std::string(wire.begin(), wire.end()).find("attack at dawn"), std::string::npos);
}

TEST_F(TlsTest, QueuesDataSentBeforeTheHandshakeCompletes) {
    auto client_context = TlsContext::client(cert.cert_path.string(), true);
    ChannelPair pair{client_context->create_channel("localhost"), server_context->create_channel(),
                     {}, {}, false};
    ASSERT_TRUE(pair.client->start());
    ASSERT_TRUE(pair.client->send(bytes_of("early")));
    pair.pump();
    ASSERT_FALSE(pair.failed);
    EXPECT_EQ(text_of(pair.server_plain), "early");
}

TEST_F(TlsTest, RejectsCertificateFromAnUntrustedIssuer) {
    orbit::testing::TempDir other_dir;
    const orbit::testing::TestCertificate other(other_dir.path());
    auto pair = connect(TlsContext::client(other.cert_path.string(), true), "localhost");
    EXPECT_TRUE(pair.failed);
}

TEST_F(TlsTest, RejectsHostnameMismatch) {
    auto pair = connect(TlsContext::client(cert.cert_path.string(), true), "example.com");
    EXPECT_TRUE(pair.failed);
}

TEST_F(TlsTest, VerificationCanBeDisabledForDevelopment) {
    auto pair = connect(TlsContext::client("", false), "anything");
    EXPECT_FALSE(pair.failed);
}

TEST_F(TlsTest, RejectsGarbage) {
    auto channel = server_context->create_channel();
    ByteBuffer plain;
    ASSERT_TRUE(channel->start());
    EXPECT_FALSE(channel->receive(bytes_of("GET / HTTP/1.1\r\n\r\n"), plain));
}

TEST(TlsContext, ReportsMissingCertificate) {
    EXPECT_THROW(TlsContext::server("/nonexistent/cert.pem", "/nonexistent/key.pem"),
                 std::runtime_error);
}
