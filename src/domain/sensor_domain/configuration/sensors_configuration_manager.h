#pragma once

#include <memory>
#include <string_view>
#include <vector>
#include <unordered_map>

#include "subsys/fs/services/i_fs_service.h"
#include "configuration/cbor/cbor_sensors_config/cbor_sensors_config.h"
#include "configuration/services/cbor_configuration_service.h"

#include "domain/configuration_domain/utilities/i_cbor_configuration_manager.h"
#include "domain/sensor_domain/configuration/parsers/sensors_cbor_parser.h"
#include "domain/sensor_domain/models/sensor.h"

namespace eerie_leap::domain::sensor_domain::configuration {

namespace config_service = eerie_leap::configuration::services;

using eerie_leap::domain::configuration_domain::utilities::ICborConfigurationManager;
using eerie_leap::domain::sensor_domain::configuration::parsers::SensorsCborParser;
using eerie_leap::domain::sensor_domain::models::Sensor;
using eerie_leap::subsys::fs::services::IFsService;

class SensorsConfigurationManager : public ICborConfigurationManager {
private:
    std::unique_ptr<config_service::CborConfigurationService<CborSensorsConfig>> cbor_configuration_service_;

    std::shared_ptr<IFsService> sd_fs_service_;

    std::unique_ptr<SensorsCborParser> cbor_parser_;

    std::vector<std::shared_ptr<Sensor>> sensors_;
    int gpio_channel_count_;
    int adc_channel_count_;

    bool CreateDefaultConfiguration();

public:
    SensorsConfigurationManager(
        std::unique_ptr<config_service::CborConfigurationService<CborSensorsConfig>> cbor_configuration_service,
        std::shared_ptr<IFsService> sd_fs_service,
        int gpio_channel_count,
        int adc_channel_count);

    bool Update(const std::vector<std::shared_ptr<Sensor>>& sensors);
    const std::vector<std::shared_ptr<Sensor>>* Get(bool force_load = false);

    bool ApplyCborConfiguration(std::span<const uint8_t> cbor_data) override;
    std::pmr::vector<uint8_t> GetCborConfiguration() override;
};

} // namespace eerie_leap::domain::sensor_domain::configuration
