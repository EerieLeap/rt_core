#include <utility>

#include <eerie_memory.hpp>

#include "domain/sensor_domain/configuration/parsers/sensor_validator.h"

#include "sensors_configuration_manager.h"

namespace eerie_leap::domain::sensor_domain::configuration {

using namespace eerie_memory;
using namespace eerie_leap::configuration::services;
using eerie_leap::domain::sensor_domain::configuration::parsers::SensorValidator;

SensorsConfigurationManager::SensorsConfigurationManager(
    std::unique_ptr<CborConfigurationService<CborSensorsConfig>> cbor_configuration_service,
    std::shared_ptr<IFsService> sd_fs_service,
    int gpio_channel_count,
    int adc_channel_count,
    CanbusConfigurationProvider canbus_configuration_provider
) :
    CborConfigurationManagerBase("Sensors", std::move(cbor_configuration_service)),
    cbor_parser_(std::move(sd_fs_service)),
    canbus_configuration_provider_(std::move(canbus_configuration_provider)),
    gpio_channel_count_(gpio_channel_count),
    adc_channel_count_(adc_channel_count) {

    LoadOrCreateDefault();
}

void SensorsConfigurationManager::RegisterConfigurationUpdatedHandler(ConfigurationUpdatedHandler handler) {
    configuration_updated_handler_ = std::move(handler);
}

bool SensorsConfigurationManager::Update(const std::vector<std::shared_ptr<Sensor>>& sensors) {
    if(!CborConfigurationManagerBase::Update(sensors))
        return false;

    if(configuration_updated_handler_)
        configuration_updated_handler_();

    return true;
}

bool SensorsConfigurationManager::ApplyCborConfiguration(std::span<const uint8_t> cbor_data, std::span<char> reason) {
    if(!CborConfigurationManagerBase::ApplyCborConfiguration(cbor_data, reason))
        return false;

    if(configuration_updated_handler_)
        configuration_updated_handler_();

    return true;
}

pmr_unique_ptr<CborSensorsConfig>
SensorsConfigurationManager::Serialize(const std::vector<std::shared_ptr<Sensor>>& sensors) {
    return cbor_parser_.Serialize(sensors, gpio_channel_count_, adc_channel_count_);
}

pmr_unique_ptr<std::vector<std::shared_ptr<Sensor>>>
SensorsConfigurationManager::Deserialize(const CborSensorsConfig& cbor_config) {
    return make_unique_pmr<std::vector<std::shared_ptr<Sensor>>>(
        Mrm::GetExtPmr(),
        cbor_parser_.Deserialize(Mrm::GetExtPmr(), cbor_config, gpio_channel_count_, adc_channel_count_)
    );
}

bool SensorsConfigurationManager::CreateDefaultConfiguration() {
    return Update({});
}

// A sensor whose CAN signal is not configured would never read. Only a client's configuration is
// refused for it: the CAN bus configuration may change after this one is stored, and the stored one
// still loads then, its sensor logging that the signal is missing or its message disabled when the
// pipeline is built.
void SensorsConfigurationManager::ValidateApply(const std::vector<std::shared_ptr<Sensor>>& sensors) {
    if(!canbus_configuration_provider_)
        return;

    const auto canbus_configuration = canbus_configuration_provider_();
    if(canbus_configuration != nullptr) {
        SensorValidator::ValidateCanbusSources(sensors, *canbus_configuration);
        return;
    }

    const CanbusConfiguration no_channels(std::allocator_arg, Mrm::GetDefaultPmr());
    SensorValidator::ValidateCanbusSources(sensors, no_channels);
}

} // namespace eerie_leap::domain::sensor_domain::configuration
