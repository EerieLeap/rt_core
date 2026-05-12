#include <zephyr/kernel.h>

#include "guid_generator.h"

namespace eerie_leap::utilities::guid {

Guid GuidGenerator::Generate() {
    atomic_inc(&counter_);
    return Guid {
        .device_hash = device_hash_,
        .counter = static_cast<uint16_t>(atomic_get(&counter_) & GuidGenerator::COUNTER_MASK),
        .timestamp = k_uptime_get_32()
    };
}

} // namespace eerie_leap::utilities::guid
