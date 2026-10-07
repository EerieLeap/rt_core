#include <zephyr/kernel.h>

#include "guid_generator.h"

namespace eerie_leap::utilities::guid {

Guid GuidGenerator::Generate() {
    // atomic_inc() returns the previous value; reading the counter again could see another thread's increment.
    const atomic_val_t counter = atomic_inc(&counter_) + 1;

    return Guid {
        .device_hash = device_hash_,
        .counter = static_cast<uint16_t>(counter & GuidGenerator::COUNTER_MASK),
        .timestamp = k_uptime_get_32()
    };
}

} // namespace eerie_leap::utilities::guid
