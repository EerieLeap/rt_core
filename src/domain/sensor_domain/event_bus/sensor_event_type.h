#pragma once

#include <cstdint>

namespace eerie_leap::domain::sensor_domain::event_bus {

enum class SensorEventType : std::uint32_t {
    DataUpdated
};

} // namespace eerie_leap::domain::sensor_domain::event_bus
