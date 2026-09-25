#pragma once

#include <memory>
#include <vector>

#include "subsys/fs/services/i_fs_service.h"
#include "configuration/cbor/cbor_sensors_config/cbor_sensors_config.h"
#include "configuration/services/cbor_configuration_service.h"

#include "domain/configuration_domain/utilities/cbor_configuration_manager_base.h"
#include "domain/sensor_domain/configuration/parsers/sensors_cbor_parser.h"
#include "domain/sensor_domain/models/sensor.h"

namespace eerie_leap::domain::sensor_domain::configuration {

namespace config_service = eerie_leap::configuration::services;

using eerie_leap::domain::configuration_domain::utilities::CborConfigurationManagerBase;
using eerie_leap::domain::sensor_domain::configuration::parsers::SensorsCborParser;
using eerie_leap::domain::sensor_domain::models::Sensor;
using eerie_leap::subsys::fs::services::IFsService;

class SensorsConfigurationManager
    : public CborConfigurationManagerBase<std::vector<std::shared_ptr<Sensor>>, CborSensorsConfig> {
private:
    SensorsCborParser cbor_parser_;

    int gpio_channel_count_;
    int adc_channel_count_;

    eerie_memory::pmr_unique_ptr<CborSensorsConfig> Serialize(const std::vector<std::shared_ptr<Sensor>>& sensors) override;
    eerie_memory::pmr_unique_ptr<std::vector<std::shared_ptr<Sensor>>> Deserialize(const CborSensorsConfig& cbor_config) override;
    bool CreateDefaultConfiguration() override;

public:
    SensorsConfigurationManager(
        std::unique_ptr<config_service::CborConfigurationService<CborSensorsConfig>> cbor_configuration_service,
        std::shared_ptr<IFsService> sd_fs_service,
        int gpio_channel_count,
        int adc_channel_count);

    const std::vector<std::shared_ptr<Sensor>>* Get(bool force_load = false);
};

} // namespace eerie_leap::domain::sensor_domain::configuration
