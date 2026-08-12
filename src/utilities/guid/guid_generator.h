#pragma once

#include <zephyr/sys/atomic.h>

#include "subsys/random/rng.h"

#include "guid.hpp"

namespace eerie_leap::utilities::guid {

using eerie_leap::subsys::random::Rng;

class GuidGenerator {
private:
    static constexpr uint16_t COUNTER_MASK = 0xFFFF;
    const uint16_t device_hash_ = Rng::Get<uint16_t>(true);
    atomic_val_t counter_ = ATOMIC_INIT(0);

public:
    GuidGenerator() = default;

    Guid Generate();
};

} // namespace eerie_leap::utilities::guid
