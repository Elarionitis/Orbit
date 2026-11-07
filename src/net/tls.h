#pragma once

#include <memory>
#include <string>

#include "net/secure_channel.h"

namespace orbit::net {

// Shared TLS configuration, wrapping an OpenSSL context. Connections are created
// from it on any thread. TLS 1.2 is the minimum version accepted.
class TlsContext {
public:
    // Throws std::runtime_error if the certificate or key cannot be loaded.
    static std::shared_ptr<TlsContext> server(const std::string& certificate_path,
                                              const std::string& key_path);

    // With an empty `ca_path` the system trust store is used. Passing
    // `verify = false` accepts any certificate: for local development only.
    static std::shared_ptr<TlsContext> client(const std::string& ca_path, bool verify);

    ~TlsContext();
    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    // `hostname` is checked against the certificate on the client side, and is
    // ignored on the server side.
    std::unique_ptr<SecureChannel> create_channel(const std::string& hostname = {}) const;

private:
    struct Impl;
    explicit TlsContext(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

}  // namespace orbit::net
