#pragma once

#include <cstddef>

namespace eerie_leap::domain::sensor_domain::models {

struct SensorLimits {
    // Upper bound of configured sensors, enforced by SensorValidator. Consumers size their
    // snapshot buffers with it so a tick never allocates.
#ifdef CONFIG_EERIE_LEAP_DOMAIN_SENSOR_MAX_COUNT
    static constexpr size_t kMaxCount = CONFIG_EERIE_LEAP_DOMAIN_SENSOR_MAX_COUNT;
#else
    static constexpr size_t kMaxCount = 64;
#endif
};

} // namespace eerie_leap::domain::sensor_domain::models
