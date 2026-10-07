#pragma once

#include <memory>

#include "domain/sensor_domain/models/sensor.h"
#include "sensor_reader_base.h"

namespace eerie_leap::domain::sensor_domain::sensor_readers {

using eerie_leap::domain::sensor_domain::models::Sensor;

class SensorReaderVirtualAnalog : public SensorReaderBase {
public:
    SensorReaderVirtualAnalog(
        std::shared_ptr<ITimeService> time_service,
        std::shared_ptr<Sensor> sensor);

    ~SensorReaderVirtualAnalog() override = default;

    SensorReading Read() override;
};

} // namespace eerie_leap::domain::sensor_domain::sensor_readers
