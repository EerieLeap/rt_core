#include <utility>

#include <eerie_memory.hpp>

#include "sensors_configuration_manager.h"

namespace eerie_leap::domain::sensor_domain::configuration {

using namespace eerie_memory;
using namespace eerie_leap::configuration::services;

SensorsConfigurationManager::SensorsConfigurationManager(
    std::unique_ptr<CborConfigurationService<CborSensorsConfig>> cbor_configuration_service,
    std::shared_ptr<IFsService> sd_fs_service,
    int gpio_channel_count,
    int adc_channel_count)
        : CborConfigurationManagerBase("Sensors", std::move(cbor_configuration_service)),
        cbor_parser_(std::move(sd_fs_service)),
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

pmr_unique_ptr<CborSensorsConfig> SensorsConfigurationManager::Serialize(const std::vector<std::shared_ptr<Sensor>>& sensors) {
    return cbor_parser_.Serialize(sensors, gpio_channel_count_, adc_channel_count_);
}

pmr_unique_ptr<std::vector<std::shared_ptr<Sensor>>> SensorsConfigurationManager::Deserialize(const CborSensorsConfig& cbor_config) {
    return make_unique_pmr<std::vector<std::shared_ptr<Sensor>>>(
        Mrm::GetExtPmr(),
        cbor_parser_.Deserialize(Mrm::GetExtPmr(), cbor_config, gpio_channel_count_, adc_channel_count_));
}

bool SensorsConfigurationManager::CreateDefaultConfiguration() {
    return Update({});
}

} // namespace eerie_leap::domain::sensor_domain::configuration
