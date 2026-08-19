#include <utility>

#include <zephyr/logging/log.h>

#include "sensors_configuration_manager.h"

namespace eerie_leap::domain::sensor_domain::configuration {

using namespace eerie_leap::configuration::services;

LOG_MODULE_REGISTER(sensors_config_ctrl_logger);

SensorsConfigurationManager::SensorsConfigurationManager(
    std::unique_ptr<CborConfigurationService<CborSensorsConfig>> cbor_configuration_service,
    std::shared_ptr<IFsService> sd_fs_service,
    int gpio_channel_count,
    int adc_channel_count)
        : cbor_configuration_service_(std::move(cbor_configuration_service)),
        sd_fs_service_(std::move(sd_fs_service)),
        gpio_channel_count_(gpio_channel_count),
        adc_channel_count_(adc_channel_count) {

    cbor_parser_ = std::make_unique<SensorsCborParser>(sd_fs_service_);

    const std::vector<std::shared_ptr<Sensor>>* sensors = nullptr;

    try {
        sensors = Get(true);
    } catch(...) {
        LOG_ERR("Failed to load Sensors configuration.");
    }

    if(sensors == nullptr) {
        LOG_ERR("Failed to load Sensors configuration.");

        if(!CreateDefaultConfiguration()) {
            LOG_ERR("Failed to create default sensors configuration.");
            return;
        }

        LOG_INF("Default Sensors configuration loaded successfully.");
    } else {
        LOG_INF("Sensors Configuration Manager initialized successfully.");
    }
}

bool SensorsConfigurationManager::ApplyCborConfiguration(std::span<const uint8_t> cbor_data) {
    auto cbor_config = cbor_configuration_service_->Deserialize(cbor_data);
    if(cbor_config == nullptr)
        return false;

    try {
        auto sensors = cbor_parser_->Deserialize(
            Mrm::GetExtPmr(),
            *cbor_config,
            gpio_channel_count_,
            adc_channel_count_);

        if(!Update(sensors))
            return false;
    } catch(const std::exception& e) {
        LOG_ERR("Failed to deserialize CBOR configuration. %s", e.what());
        return false;
    }

    LOG_INF("CBOR configuration loaded successfully.");

    return true;
}

std::pmr::vector<uint8_t> SensorsConfigurationManager::GetCborConfiguration() {
    auto cbor_config = cbor_parser_->Serialize(
        sensors_,
        gpio_channel_count_,
        adc_channel_count_);

    return cbor_configuration_service_->Serialize(*cbor_config);
}

bool SensorsConfigurationManager::Update(const std::vector<std::shared_ptr<Sensor>>& sensors) {
    try {
        auto cbor_config = cbor_parser_->Serialize(
            sensors,
            gpio_channel_count_,
            adc_channel_count_);

        if(!cbor_configuration_service_->Save(cbor_config.get()))
            return false;
    } catch(const std::exception& e) {
        LOG_ERR("Failed to update Sensors configuration. %s", e.what());
        return false;
    }

    return Get(true) != nullptr;
}

const std::vector<std::shared_ptr<Sensor>>* SensorsConfigurationManager::Get(bool force_load) {
    if(!sensors_.empty() && !force_load)
        return &sensors_;

    auto cbor_config_data = cbor_configuration_service_->Load();
    if(!cbor_config_data.has_value())
        return nullptr;

    auto cbor_config = std::move(cbor_config_data.value().config);

    sensors_.clear();
    sensors_ = cbor_parser_->Deserialize(
        Mrm::GetExtPmr(), *cbor_config, gpio_channel_count_, adc_channel_count_);

    return &sensors_;
}

bool SensorsConfigurationManager::CreateDefaultConfiguration() {
    auto sensors = std::vector<std::shared_ptr<Sensor>>();

    return Update(sensors);
}

} // namespace eerie_leap::domain::sensor_domain::configuration
