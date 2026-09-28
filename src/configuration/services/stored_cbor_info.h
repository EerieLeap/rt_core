#pragma once

#include <cstddef>
#include <cstdint>

namespace eerie_leap::configuration::services {

/// Size and CRC32 of a stored CBOR configuration.
struct StoredCborInfo {
    size_t size = 0;
    uint32_t crc = 0;
};

} // namespace eerie_leap::configuration::services
