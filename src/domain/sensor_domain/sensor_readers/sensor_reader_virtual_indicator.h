#pragma once

#include <memory>

#include "sensor_reader_base.h"

namespace eerie_leap::domain::sensor_domain::sensor_readers {

class SensorReaderVirtualIndicator : public SensorReaderBase {
public:
    SensorReaderVirtualIndicator(
        std::shared_ptr<ITimeService> time_service,
        const SensorRuntime& runtime);

    ~SensorReaderVirtualIndicator() override = default;

    SensorReading Read() override;
};

} // namespace eerie_leap::domain::sensor_domain::sensor_readers
