#pragma once

#include <cstdint>

namespace eerie_leap::domain::sensor_domain::event_bus {

enum class SensorPayloadType : std::uint32_t {
    SensorId,
    Value
};

} // namespace eerie_leap::domain::sensor_domain::event_bus
