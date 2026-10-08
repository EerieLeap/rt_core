#pragma once

#include <functional>

#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/sensor_domain/runtime/sensor_runtime.h"

namespace eerie_leap::domain::sensor_domain::isr_sensor_readers {

using eerie_leap::domain::sensor_domain::models::SensorReading;
using eerie_leap::domain::sensor_domain::runtime::SensorRuntime;

// Receives a reading the reader produced, on the reader's work queue, and commits it.
using ProcessSensorCallback = std::function<void(const SensorRuntime&, SensorReading&)>;

class IIsrSensorReader {
public:
    virtual ~IIsrSensorReader() = default;
};

} // namespace eerie_leap::domain::sensor_domain::isr_sensor_readers
