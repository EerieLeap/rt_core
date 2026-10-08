#include <stdexcept>

#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/sensor_domain/models/reading_status.h"
#include "sensor_reader_physical_analog.h"

namespace eerie_leap::domain::sensor_domain::sensor_readers {

using namespace eerie_leap::subsys::adc::utilities;
using namespace eerie_leap::domain::sensor_domain::models;

SensorReaderPhysicalAnalog::SensorReaderPhysicalAnalog(
    std::shared_ptr<ITimeService> time_service,
    const SensorRuntime& runtime,
    std::shared_ptr<AdcConfigurationManager> adc_configuration_manager)
        : SensorReaderBase(std::move(time_service), runtime),
        adc_configuration_manager_(std::move(adc_configuration_manager)) {

    const auto& configuration = GetSensor().configuration;

    if(configuration.type != SensorType::PHYSICAL_ANALOG)
        throw std::runtime_error("Unsupported sensor type");

    adc_manager_ = adc_configuration_manager_->Get();
    adc_channel_configuration_ = adc_manager_->GetChannelConfiguration(configuration.channel.value());
    AdcChannelReader = adc_manager_->GetChannelReader(configuration.channel.value());
}

SensorReading SensorReaderPhysicalAnalog::Read() {
    SensorReading reading = CreateReading();

    float voltage = AdcChannelReader();
    float voltage_calibrated = adc_channel_configuration_->calibrator->InterpolateToCalibratedRange(voltage);
    reading.voltage = voltage_calibrated;

    if(runtime_->voltage_interpolator == nullptr)
        throw std::runtime_error("Sensor has no interpolator");

    reading.value = runtime_->voltage_interpolator->Interpolate(voltage_calibrated, true);
    reading.raw_value = reading.value;

    reading.status = ReadingStatus::INTERPOLATED;

    return reading;
}

} // namespace eerie_leap::domain::sensor_domain::sensor_readers
