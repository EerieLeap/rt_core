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

const std::vector<std::shared_ptr<Sensor>>* SensorsConfigurationManager::Get(bool force_load) {
    return CborConfigurationManagerBase::Get(force_load).get();
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
