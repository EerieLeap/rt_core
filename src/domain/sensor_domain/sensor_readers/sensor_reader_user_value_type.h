#pragma once

#include <memory>

#include "sensor_reader_base.h"

namespace eerie_leap::domain::sensor_domain::sensor_readers {

// Reads a USER sensor by calling the script's create_sensor_value(sensor_id), which returns the
// value. Without the function the reading stays UNINITIALIZED.
class SensorReaderUserValueType : public SensorReaderBase {
public:
    SensorReaderUserValueType(
        std::shared_ptr<ITimeService> time_service,
        const SensorRuntime& runtime);

    ~SensorReaderUserValueType() override = default;

    SensorReading Read() override;
};

} // namespace eerie_leap::domain::sensor_domain::sensor_readers
