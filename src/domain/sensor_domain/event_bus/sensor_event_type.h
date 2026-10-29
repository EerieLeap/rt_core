#pragma once

#include <cstdint>

namespace eerie_leap::domain::sensor_domain::event_bus {

// Persisted as the event of a widget binding or a logging trigger: append only.
enum class SensorEventType : std::uint32_t {
    // Sensor domain -> anyone: a committed reading {SensorId, Value}. The fact.
    DataUpdated,

    // A control -> the sensor domain: the input of a control sensor {SensorId, Value}, which its
    // pipeline processes. Only DataUpdated is a fact.
    InputRequested
};

// The channel bounds HasEventType() with it; appending a value means moving this to it.
inline constexpr SensorEventType LAST_SENSOR_EVENT_TYPE = SensorEventType::InputRequested;

} // namespace eerie_leap::domain::sensor_domain::event_bus
