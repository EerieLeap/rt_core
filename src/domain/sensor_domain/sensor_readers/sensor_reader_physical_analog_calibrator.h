#pragma once

#include "sensor_reader_physical_analog.h"

namespace eerie_leap::domain::sensor_domain::sensor_readers {

class SensorReaderPhysicalAnalogCalibrator : public SensorReaderPhysicalAnalog {
public:
    SensorReaderPhysicalAnalogCalibrator(
        std::shared_ptr<ITimeService> time_service,
        const SensorRuntime& runtime,
        std::shared_ptr<AdcConfigurationManager> adc_configuration_manager);

    ~SensorReaderPhysicalAnalogCalibrator() override = default;

    SensorReading Read() override;
};

} // namespace eerie_leap::domain::sensor_domain::sensor_readers
