#pragma once

#include <cstdint>

namespace eerie_leap::domain::sensor_domain::event_bus {

enum class SensorPayloadType : std::uint32_t {
    SensorId,
    Value
};

// The channel bounds HasPayloadKey() with it; appending a value means moving this to it.
inline constexpr SensorPayloadType LAST_SENSOR_PAYLOAD_TYPE = SensorPayloadType::Value;

} // namespace eerie_leap::domain::sensor_domain::event_bus
