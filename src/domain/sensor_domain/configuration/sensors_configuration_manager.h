#pragma once

#include <memory>
#include <string_view>
#include <vector>
#include <unordered_map>

#include "subsys/fs/services/i_fs_service.h"
#include "configuration/cbor/cbor_sensors_config/cbor_sensors_config.h"
#include "configuration/services/cbor_configuration_service.h"
#include "configuration/json/configs/json_sensors_config.h"
#include "configuration/services/json_configuration_service.h"

#include "domain/configuration_domain/utilities/i_json_configuration_manager.h"
#include "domain/sensor_domain/configuration/parsers/sensors_cbor_parser.h"
#include "domain/sensor_domain/configuration/parsers/sensors_json_parser.h"
#include "domain/sensor_domain/models/sensor.h"

namespace eerie_leap::domain::sensor_domain::configuration {

namespace config_service = eerie_leap::configuration::services;

using eerie_leap::configuration::json::configs::JsonSensorsConfig;
using eerie_leap::domain::configuration_domain::utilities::IJsonConfigurationManager;
using eerie_leap::domain::sensor_domain::configuration::parsers::SensorsCborParser;
using eerie_leap::domain::sensor_domain::configuration::parsers::SensorsJsonParser;
using eerie_leap::domain::sensor_domain::models::Sensor;
using eerie_leap::subsys::fs::services::IFsService;

class SensorsConfigurationManager : public IJsonConfigurationManager {
private:
    std::unique_ptr<config_service::CborConfigurationService<CborSensorsConfig>> cbor_configuration_service_;
    std::unique_ptr<config_service::JsonConfigurationService<JsonSensorsConfig>> json_configuration_service_;

    std::shared_ptr<IFsService> sd_fs_service_;

    std::unique_ptr<SensorsCborParser> cbor_parser_;
    std::unique_ptr<SensorsJsonParser> json_parser_;

    std::vector<std::shared_ptr<Sensor>> sensors_;
    int gpio_channel_count_;
    int adc_channel_count_;

    uint32_t json_config_checksum_;

    bool ApplyJsonConfiguration(bool fs_load, std::string_view json_str = {});
    bool CreateDefaultConfiguration();

public:
    SensorsConfigurationManager(
        std::unique_ptr<config_service::CborConfigurationService<CborSensorsConfig>> cbor_configuration_service,
        std::unique_ptr<config_service::JsonConfigurationService<JsonSensorsConfig>> json_configuration_service,
        std::shared_ptr<IFsService> sd_fs_service,
        int gpio_channel_count,
        int adc_channel_count);

    bool Update(const std::vector<std::shared_ptr<Sensor>>& sensors, bool internal_only = false);
    const std::vector<std::shared_ptr<Sensor>>* Get(bool force_load = false);

    bool ApplyJsonConfiguration(std::string_view json_str) override;
    std::pmr::string GetJsonConfiguration() override;
};

} // namespace eerie_leap::domain::sensor_domain::configuration
