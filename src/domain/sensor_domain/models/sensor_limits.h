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

    // Raw CAN frames queued for the log writer between its ticks.
#ifdef CONFIG_EERIE_LEAP_DOMAIN_SENSOR_RAW_CAN_QUEUE_SIZE
    static constexpr size_t kRawCanQueueSize = CONFIG_EERIE_LEAP_DOMAIN_SENSOR_RAW_CAN_QUEUE_SIZE;
#else
    static constexpr size_t kRawCanQueueSize = 32;
#endif
};

} // namespace eerie_leap::domain::sensor_domain::models
