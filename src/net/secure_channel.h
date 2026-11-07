#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "common/byte_buffer.h"

namespace orbit::net {

// A byte-stream transform sitting between a Connection and its socket. Written
// sans-IO: it only ever sees buffers, so the same socket code serves plain and
// encrypted connections, and the reactor never blocks inside a TLS library.
class SecureChannel {
public:
    virtual ~SecureChannel() = default;

    // Begins the handshake if this side speaks first.
    virtual bool start() = 0;

    // Consumes bytes from the socket and appends any plaintext to `plaintext`.
    // Returns false on a fatal protocol error.
    virtual bool receive(std::span<const std::uint8_t> ciphertext, ByteBuffer& plaintext) = 0;

    // Queues plaintext to send. Returns false on a fatal protocol error.
    virtual bool send(std::span<const std::uint8_t> plaintext) = 0;

    // Moves bytes ready for the socket into `ciphertext`.
    virtual void take_output(std::vector<std::uint8_t>& ciphertext) = 0;
};

}  // namespace orbit::net
