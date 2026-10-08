#include "domain/sensor_domain/models/sensor_type_traits.h"
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

std::unique_ptr<ISensorReader> SensorReaderFactory::Create(const SensorRuntime& runtime) {
    const auto traits = runtime.GetSensor().configuration.GetTraits();

    switch(traits.source) {
    case SensorSourceKind::ADC:
        if(adc_configuration_manager_ == nullptr)
            return nullptr;

        return std::make_unique<SensorReaderPhysicalAnalog>(time_service_, runtime, adc_configuration_manager_);

    case SensorSourceKind::GPIO:
        if(gpio_ == nullptr)
            return nullptr;

        return std::make_unique<SensorReaderPhysicalIndicator>(time_service_, runtime, gpio_);

    case SensorSourceKind::EXPRESSION:
        if(traits.value_kind == SensorValueKind::INDICATOR)
            return std::make_unique<SensorReaderVirtualIndicator>(time_service_, runtime);

        return std::make_unique<SensorReaderVirtualAnalog>(time_service_, runtime);

    case SensorSourceKind::SCRIPT:
        return std::make_unique<SensorReaderUserValueType>(time_service_, runtime);

    default:
        return nullptr;
    }
}

} // namespace eerie_leap::domain::sensor_domain::sensor_readers
