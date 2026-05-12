#pragma once

#include <functional>

#include "domain/sensor_domain/models/sensor.h"

namespace eerie_leap::domain::sensor_domain::isr_sensor_readers {

using eerie_leap::domain::sensor_domain::models::Sensor;

using ProcessSensorCallback = std::function<void(const Sensor&)>;

class IIsrSensorReader {
public:
    virtual ~IIsrSensorReader() = default;
};

} // namespace eerie_leap::domain::sensor_domain::isr_sensor_readers
