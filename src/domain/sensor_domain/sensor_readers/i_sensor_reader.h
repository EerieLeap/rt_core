#pragma once

#include "domain/sensor_domain/models/sensor_reading.h"

namespace eerie_leap::domain::sensor_domain::sensor_readers {

using eerie_leap::domain::sensor_domain::models::SensorReading;

class ISensorReader {
public:
    virtual ~ISensorReader() = default;

    // Produces the sensor's next reading. May throw when the source fails; the caller records that.
    virtual SensorReading Read() = 0;
};

} // namespace eerie_leap::domain::sensor_domain::sensor_readers
