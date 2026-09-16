#pragma once

#include <cstdint>

namespace eerie_leap::domain::sensor_domain::models {

// Persisted IDs: append new types before COUNT; never reorder or reuse values.
enum class SensorType : std::uint32_t {
    NONE = 0,
    PHYSICAL_ANALOG,
    VIRTUAL_ANALOG,
    PHYSICAL_INDICATOR,
    VIRTUAL_INDICATOR,
    CANBUS_RAW,
    CANBUS_ANALOG,
    CANBUS_INDICATOR,
    USER_ANALOG,
    USER_INDICATOR,
    COUNT // Sentinel, not a sensor type
};

constexpr bool IsSensorTypeValid(SensorType type) {
    return type < SensorType::COUNT;
}

} // namespace eerie_leap::domain::sensor_domain::models
