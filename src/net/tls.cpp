#include "net/tls.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <array>
#include <format>
#include <stdexcept>

#include "common/log.h"

namespace orbit::net {
namespace {

std::string openssl_error() {
    std::array<char, 256> text{};
    ERR_error_string_n(ERR_get_error(), text.data(), text.size());
    return text.data();
}

// Both BIOs are in-memory, so OpenSSL never touches the socket. It reads
// ciphertext from one buffer and writes ciphertext to another, and the reactor
// moves bytes between those buffers and the network. WANT_READ and WANT_WRITE
// therefore never bubble up into the I/O path.
class OpenSslChannel final : public SecureChannel {
public:
    OpenSslChannel(SSL_CTX* context, bool is_server, const std::string& hostname)
        : m_ssl(SSL_new(context)), m_is_server(is_server) {
        if (m_ssl == nullptr) {
            throw std::runtime_error("SSL_new: " + openssl_error());
        }
        m_in = BIO_new(BIO_s_mem());
        m_out = BIO_new(BIO_s_mem());
        SSL_set_bio(m_ssl, m_in, m_out);  // m_ssl now owns both BIOs.

        if (is_server) {
            SSL_set_accept_state(m_ssl);
        } else {
            SSL_set_connect_state(m_ssl);
            if (!hostname.empty()) {
                SSL_set_tlsext_host_name(m_ssl, hostname.c_str());
                SSL_set1_host(m_ssl, hostname.c_str());
            }
        }
    }

    ~OpenSslChannel() override { SSL_free(m_ssl); }

    bool start() override { return m_is_server || advance_handshake(); }

    bool receive(std::span<const std::uint8_t> ciphertext, ByteBuffer& plaintext) override {
        if (BIO_write(m_in, ciphertext.data(), static_cast<int>(ciphertext.size())) < 0) {
            return false;
        }
        if (!m_established && !advance_handshake()) {
            return false;
        }
        if (!m_established) {
            return true;
        }
        for (;;) {
            auto target = plaintext.prepare(16 * 1024);
            const int count = SSL_read(m_ssl, target.data(), static_cast<int>(target.size()));
            if (count > 0) {
                plaintext.commit(static_cast<std::size_t>(count));
                continue;
            }
            const int error = SSL_get_error(m_ssl, count);
            // ZERO_RETURN is the peer's close_notify; the TCP EOF follows it.
            return error == SSL_ERROR_WANT_READ || error == SSL_ERROR_ZERO_RETURN;
        }
    }

    bool send(std::span<const std::uint8_t> plaintext) override {
        if (!m_established) {
            m_pending.append(plaintext);
            return true;
        }
        return write_all(plaintext);
    }

    void take_output(std::vector<std::uint8_t>& ciphertext) override {
        const std::size_t pending = BIO_ctrl_pending(m_out);
        if (pending == 0) {
            return;
        }
        ciphertext.resize(pending);
        const int count = BIO_read(m_out, ciphertext.data(), static_cast<int>(pending));
        ciphertext.resize(count > 0 ? static_cast<std::size_t>(count) : 0);
    }

private:
    bool advance_handshake() {
        const int result = SSL_do_handshake(m_ssl);
        if (result == 1) {
            m_established = true;
            const bool sent = write_all(m_pending.readable());
            m_pending.clear();
            return sent;
        }
        const int error = SSL_get_error(m_ssl, result);
        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
            return true;
        }
        log::debug("TLS handshake failed: {}", openssl_error());
        return false;
    }

    bool write_all(std::span<const std::uint8_t> plaintext) {
        if (plaintext.empty()) {
            return true;
        }
        // With a memory BIO the write cannot be short: the BIO simply grows.
        return SSL_write(m_ssl, plaintext.data(), static_cast<int>(plaintext.size())) ==
               static_cast<int>(plaintext.size());
    }

    SSL* m_ssl;
    BIO* m_in = nullptr;
    BIO* m_out = nullptr;
    bool m_is_server;
    bool m_established = false;
    ByteBuffer m_pending;  // Plaintext queued before the handshake finished.
};

}  // namespace

struct TlsContext::Impl {
    SSL_CTX* context = nullptr;
    bool is_server = false;

    ~Impl() { SSL_CTX_free(context); }
};

TlsContext::TlsContext(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
TlsContext::~TlsContext() = default;

std::shared_ptr<TlsContext> TlsContext::server(const std::string& certificate_path,
                                               const std::string& key_path) {
    auto impl = std::make_unique<Impl>();
    impl->is_server = true;
    impl->context = SSL_CTX_new(TLS_server_method());
    if (impl->context == nullptr) {
        throw std::runtime_error("SSL_CTX_new: " + openssl_error());
    }
    SSL_CTX_set_min_proto_version(impl->context, TLS1_2_VERSION);
    if (SSL_CTX_use_certificate_chain_file(impl->context, certificate_path.c_str()) != 1) {
        throw std::runtime_error(
            std::format("cannot load certificate {}: {}", certificate_path, openssl_error()));
    }
    if (SSL_CTX_use_PrivateKey_file(impl->context, key_path.c_str(), SSL_FILETYPE_PEM) != 1) {
        throw std::runtime_error(std::format("cannot load key {}: {}", key_path, openssl_error()));
    }
    if (SSL_CTX_check_private_key(impl->context) != 1) {
        throw std::runtime_error("TLS private key does not match the certificate");
    }
    return std::shared_ptr<TlsContext>(new TlsContext(std::move(impl)));
}

std::shared_ptr<TlsContext> TlsContext::client(const std::string& ca_path, bool verify) {
    auto impl = std::make_unique<Impl>();
    impl->context = SSL_CTX_new(TLS_client_method());
    if (impl->context == nullptr) {
        throw std::runtime_error("SSL_CTX_new: " + openssl_error());
    }
    SSL_CTX_set_min_proto_version(impl->context, TLS1_2_VERSION);
    if (verify) {
        SSL_CTX_set_verify(impl->context, SSL_VERIFY_PEER, nullptr);
        const int loaded = ca_path.empty()
                               ? SSL_CTX_set_default_verify_paths(impl->context)
                               : SSL_CTX_load_verify_locations(impl->context, ca_path.c_str(), nullptr);
        if (loaded != 1) {
            throw std::runtime_error("cannot load CA certificates: " + openssl_error());
        }
    } else {
        SSL_CTX_set_verify(impl->context, SSL_VERIFY_NONE, nullptr);
    }
    return std::shared_ptr<TlsContext>(new TlsContext(std::move(impl)));
}

std::unique_ptr<SecureChannel> TlsContext::create_channel(const std::string& hostname) const {
    return std::make_unique<OpenSslChannel>(m_impl->context, m_impl->is_server, hostname);
}

}  // namespace orbit::net
