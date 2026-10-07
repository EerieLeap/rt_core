#include "domain/sensor_domain/models/sensor_type.h"
#include "domain/sensor_domain/sensor_readers/sensor_reader_physical_analog.h"
#include "domain/sensor_domain/sensor_readers/sensor_reader_physical_indicator.h"
#include "domain/sensor_domain/sensor_readers/sensor_reader_virtual_analog.h"
#include "domain/sensor_domain/sensor_readers/sensor_reader_virtual_indicator.h"
#include "domain/sensor_domain/sensor_readers/sensor_reader_user_value_type.h"

#include "sensor_reader_factory.h"

namespace eerie_leap::domain::sensor_domain::sensor_readers {

using namespace eerie_leap::domain::sensor_domain::models;

SensorReaderFactory::SensorReaderFactory(
    std::shared_ptr<ITimeService> time_service,
    std::shared_ptr<IGpio> gpio,
    std::shared_ptr<AdcConfigurationManager> adc_configuration_manager)
        : time_service_(std::move(time_service)),
        gpio_(std::move(gpio)),
        adc_configuration_manager_(std::move(adc_configuration_manager)) {}

std::unique_ptr<ISensorReader> SensorReaderFactory::Create(std::shared_ptr<Sensor> sensor) {
    std::unique_ptr<ISensorReader> sensor_reader;

    if(sensor->configuration.type == SensorType::PHYSICAL_ANALOG) {
        if(adc_configuration_manager_ == nullptr)
            return nullptr;

        sensor_reader = std::make_unique<SensorReaderPhysicalAnalog>(
            time_service_,
            sensor,
            adc_configuration_manager_);
    } else if(sensor->configuration.type == SensorType::VIRTUAL_ANALOG) {
        sensor_reader = std::make_unique<SensorReaderVirtualAnalog>(
            time_service_,
            sensor);
    } else if(sensor->configuration.type == SensorType::PHYSICAL_INDICATOR) {
        if(gpio_ == nullptr)
            return nullptr;

        sensor_reader = std::make_unique<SensorReaderPhysicalIndicator>(
            time_service_,
            sensor,
            gpio_);
    } else if(sensor->configuration.type == SensorType::VIRTUAL_INDICATOR) {
        sensor_reader = std::make_unique<SensorReaderVirtualIndicator>(
            time_service_,
            sensor);
    } else if(sensor->configuration.type == SensorType::USER_ANALOG || sensor->configuration.type == SensorType::USER_INDICATOR) {
        sensor_reader = std::make_unique<SensorReaderUserValueType>(
            time_service_,
            sensor);
    } else {
        return nullptr;
    }

    return sensor_reader;
}

} // namespace eerie_leap::domain::sensor_domain::sensor_readers
