// Feeds arbitrary bytes through the stream decoder exactly as a socket would.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "common/byte_buffer.h"
#include "protocol/messages.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace orbit::protocol;

    // The first byte picks a fragmentation size, so the fuzzer also explores
    // frames that arrive split across reads.
    if (size == 0) {
        return 0;
    }
    const std::size_t chunk = static_cast<std::size_t>(data[0]) + 1;
    const std::span<const std::uint8_t> input(data + 1, size - 1);

    orbit::ByteBuffer buffer;
    for (std::size_t offset = 0; offset < input.size(); offset += chunk) {
        buffer.append(input.subspan(offset, std::min(chunk, input.size() - offset)));
        for (;;) {
            const auto frame = extract_frame(buffer.readable(), kDefaultMaxFrameSize);
            if (frame.status == ExtractStatus::Oversized || frame.status == ExtractStatus::Empty) {
                return 0;
            }
            if (frame.status == ExtractStatus::NeedMore) {
                break;
            }
            if (const auto message = decode(frame.body)) {
                // Canonical encoding: an accepted message must survive a round trip.
                const auto again = encode(*message);
                const auto reparsed =
                    decode(std::span<const std::uint8_t>(again).subspan(kLengthPrefixSize));
                if (!reparsed || !(*reparsed == *message)) {
                    std::abort();
                }
            }
            buffer.consume(frame.consumed);
        }
    }
    return 0;
}
