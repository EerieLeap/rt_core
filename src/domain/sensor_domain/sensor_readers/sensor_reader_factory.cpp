#include <stdexcept>
#include <string>

#include "domain/sensor_domain/models/sensor_type_traits.h"
#include "domain/sensor_domain/sensor_readers/sensor_reader_physical_analog.h"
#include "domain/sensor_domain/sensor_readers/sensor_reader_physical_indicator.h"
#include "domain/sensor_domain/sensor_readers/sensor_reader_user_value_type.h"
#include "domain/sensor_domain/sensor_readers/sensor_reader_virtual_analog.h"
#include "domain/sensor_domain/sensor_readers/sensor_reader_virtual_indicator.h"

#include "sensor_reader_factory.h"

namespace eerie_leap::domain::sensor_domain::sensor_readers {

using namespace eerie_leap::subsys::adc;
using namespace eerie_leap::domain::sensor_domain::models;

SensorReaderFactory::SensorReaderFactory(
    std::shared_ptr<ITimeService> time_service,
    std::shared_ptr<IGpio> gpio,
    std::shared_ptr<IAdcManager> adc_manager
) :
    time_service_(std::move(time_service)),
    gpio_(std::move(gpio)),
    adc_manager_(std::move(adc_manager)) {}

std::unique_ptr<ISensorReader>
SensorReaderFactory::Create(const SensorRuntime& runtime, RateGroupBuildContext& context) {
    const auto traits = runtime.GetSensor().configuration.GetTraits();

    switch(traits.source) {
        case SensorSourceKind::ADC:
            if(adc_manager_ == nullptr)
                return nullptr;

            return CreatePhysicalAnalog(runtime, context);

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

// The hardware channel and the calibrator are resolved here, once; the reader only reads its slot.
std::unique_ptr<ISensorReader>
SensorReaderFactory::CreatePhysicalAnalog(const SensorRuntime& runtime, RateGroupBuildContext& context) {

    const Sensor& sensor = runtime.GetSensor();
    const int channel = sensor.configuration.channel.value();

    const AdcChannelHandle handle = adc_manager_->Resolve(channel);

    const auto* adc_configuration = context.adc_configuration;
    if(adc_configuration == nullptr || adc_configuration->channel_configurations == nullptr)
        throw std::invalid_argument("Sensor " + std::string(sensor.id) + ": no ADC configuration.");

    const auto& channel_configurations = *adc_configuration->channel_configurations;
    if(static_cast<size_t>(channel) >= channel_configurations.size()
        || channel_configurations[channel] == nullptr
        || channel_configurations[channel]->calibrator == nullptr) {

        throw std::invalid_argument(
            "Sensor " + std::string(sensor.id) + ": ADC channel " + std::to_string(channel) + " has no calibration."
        );
    }

    const AdcSample* sample = context.adc_samples.Reserve(channel, handle, adc_configuration->samples);

    return std::make_unique<SensorReaderPhysicalAnalog>(
        time_service_, runtime, sample, channel_configurations[channel]->calibrator
    );
}

} // namespace eerie_leap::domain::sensor_domain::sensor_readers
