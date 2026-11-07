// Fuzzes log recovery. Random bytes alone would die at the header or the first CRC
// and test nothing, so the harness writes a valid header and frames the input into
// records with correct checksums. That carries the mutations through to the record
// parser, while a control byte per record can still break a checksum or tear the
// tail. Recovery must survive any of it and be stable: a second pass over what the
// first left behind has to agree and truncate nothing further.
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "common/crc32c.h"
#include "common/log.h"
#include "storage/wal.h"

namespace fs = std::filesystem;

namespace {

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

std::vector<std::uint8_t> build_segment(std::span<const std::uint8_t> input) {
    std::vector<std::uint8_t> segment = {'O', 'R', 'B', 'I', 'T', 'W', 'A', 'L'};
    put_u32(segment, 1);  // Format version.
    put_u32(segment, 0);  // Reserved.

    while (input.size() >= 2) {
        const std::uint8_t control = input[0];
        const std::size_t length = std::min<std::size_t>(input[1], input.size() - 2);
        const auto payload = input.subspan(2, length);
        input = input.subspan(2 + length);

        const bool break_checksum = (control & 0x01U) != 0;
        put_u32(segment, static_cast<std::uint32_t>(length));
        put_u32(segment, orbit::crc32c(payload) ^ (break_checksum ? 1U : 0U));
        segment.insert(segment.end(), payload.begin(), payload.end());
        if ((control & 0x02U) != 0 && !payload.empty()) {
            segment.pop_back();  // A torn final write.
            break;
        }
    }
    return segment;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using orbit::storage::Wal;
    using orbit::storage::WalRecord;

    orbit::log::set_level(orbit::log::Level::Off);
    static const fs::path directory =
        fs::temp_directory_path() / ("orbit-fuzz-wal-" + std::to_string(::getpid()));
    fs::create_directories(directory);

    const auto segment = build_segment({data, size});
    {
        std::ofstream file(directory / "wal-0000000000000001.log",
                           std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(segment.data()),
                   static_cast<std::streamsize>(segment.size()));
    }

    std::size_t first_pass = 0;
    Wal::recover(directory, [&](const WalRecord&) { ++first_pass; });

    std::size_t second_pass = 0;
    const auto report = Wal::recover(directory, [&](const WalRecord&) { ++second_pass; });
    if (first_pass != second_pass || report.truncated_bytes != 0) {
        std::abort();
    }
    return 0;
}
