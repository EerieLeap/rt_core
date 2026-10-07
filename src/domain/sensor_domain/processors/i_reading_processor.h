#pragma once

#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/models/sensor_reading.h"

namespace eerie_leap::domain::sensor_domain::processors {

using eerie_leap::domain::sensor_domain::models::Sensor;
using eerie_leap::domain::sensor_domain::models::SensorReading;

// A stage of the processing pipeline. It changes the reading it is given and must not throw;
// a failure is recorded on the reading with SensorReading::SetError().
class IReadingProcessor {
public:
    virtual ~IReadingProcessor() = default;

    virtual void Process(const Sensor& sensor, SensorReading& reading) = 0;
};

} // namespace eerie_leap::domain::sensor_domain::processors
