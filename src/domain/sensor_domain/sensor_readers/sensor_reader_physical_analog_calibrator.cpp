#include <stdexcept>

#include "subsys/adc/utilities/adc_calibrator.h"
#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/sensor_domain/models/reading_status.h"
#include "sensor_reader_physical_analog_calibrator.h"

namespace eerie_leap::domain::sensor_domain::sensor_readers {

using namespace eerie_leap::subsys::adc::utilities;
using namespace eerie_leap::domain::sensor_domain::models;

SensorReaderPhysicalAnalogCalibrator::SensorReaderPhysicalAnalogCalibrator(
    std::shared_ptr<ITimeService> time_service,
    const SensorRuntime& runtime,
    std::shared_ptr<AdcConfigurationManager> adc_configuration_manager)
        : SensorReaderPhysicalAnalog(
            std::move(time_service),
            runtime,
            std::move(adc_configuration_manager)) { }

SensorReading SensorReaderPhysicalAnalogCalibrator::Read() {
    SensorReading reading = CreateReading();

    float voltage = AdcChannelReader();
    float voltage_interpolated = AdcCalibrator::InterpolateToInputRange(voltage);

    reading.voltage = voltage;
    reading.value = voltage_interpolated;
    reading.raw_value = voltage_interpolated;
    reading.status = ReadingStatus::CALIBRATION;

    return reading;
}

} // namespace eerie_leap::domain::sensor_domain::sensor_readers
