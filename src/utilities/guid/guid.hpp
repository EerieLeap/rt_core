#pragma once

#include <cstdint>
#include <cstring>

namespace eerie_leap::utilities::guid {

struct Guid {
    uint16_t device_hash;
    uint16_t counter;
    uint32_t timestamp;

    uint64_t AsUint64() const {
        uint64_t value;
        memcpy(&value, this, sizeof(*this));

        return value;
    }
} __attribute__((packed, aligned(1))); // Ensure no padding

static_assert(sizeof(Guid) == sizeof(uint64_t), "Guid must pack into a uint64_t");

} // namespace eerie_leap::utilities::guid
