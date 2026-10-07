#pragma once

#include <functional>

#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/models/sensor_reading.h"

namespace eerie_leap::domain::sensor_domain::isr_sensor_readers {

using eerie_leap::domain::sensor_domain::models::Sensor;
using eerie_leap::domain::sensor_domain::models::SensorReading;

// Receives a reading the reader produced, on the reader's work queue, and commits it.
using ProcessSensorCallback = std::function<void(const Sensor&, SensorReading&)>;

class IIsrSensorReader {
public:
    virtual ~IIsrSensorReader() = default;
};

} // namespace eerie_leap::domain::sensor_domain::isr_sensor_readers
