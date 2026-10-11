#pragma once

#include "domain/sensor_domain/models/sensor_reading.h"
#include "tick_context.h"

namespace eerie_leap::domain::sensor_domain::sensor_readers {

using eerie_leap::domain::sensor_domain::models::SensorReading;

class ISensorReader {
public:
    virtual ~ISensorReader() = default;

    // Produces the sensor's reading for the tick, stamped with the tick's time. A failed source
    // gives an error reading; a reader may still throw, and the caller records that.
    virtual SensorReading Read(const TickContext& tick) = 0;
};

} // namespace eerie_leap::domain::sensor_domain::sensor_readers
