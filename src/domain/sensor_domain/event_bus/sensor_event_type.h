#pragma once

#include <cstdint>

namespace eerie_leap::domain::sensor_domain::event_bus {

enum class SensorEventType : std::uint32_t {
    DataUpdated
};

// The channel bounds HasEventType() with it; appending a value means moving this to it.
inline constexpr SensorEventType LAST_SENSOR_EVENT_TYPE = SensorEventType::DataUpdated;

} // namespace eerie_leap::domain::sensor_domain::event_bus
