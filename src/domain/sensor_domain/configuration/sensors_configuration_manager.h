#pragma once

#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "configuration/cbor/cbor_sensors_config/cbor_sensors_config.h"
#include "configuration/services/cbor_configuration_service.h"
#include "subsys/fs/services/i_fs_service.h"

#include "domain/canbus_domain/models/canbus_configuration.h"

#include "domain/configuration_domain/utilities/cbor_configuration_manager_base.h"
#include "domain/configuration_domain/utilities/i_configuration_manager.h"
#include "domain/sensor_domain/configuration/parsers/sensors_cbor_parser.h"
#include "domain/sensor_domain/models/sensor.h"

namespace eerie_leap::domain::sensor_domain::configuration {

namespace config_service = eerie_leap::configuration::services;

using eerie_leap::domain::canbus_domain::models::CanbusConfiguration;
using eerie_leap::domain::configuration_domain::utilities::CborConfigurationManagerBase;
using eerie_leap::domain::configuration_domain::utilities::IConfigurationManager;
using eerie_leap::domain::sensor_domain::configuration::parsers::SensorsCborParser;
using eerie_leap::domain::sensor_domain::models::Sensor;
using eerie_leap::subsys::fs::services::IFsService;

class SensorsConfigurationManager :
    public CborConfigurationManagerBase<std::vector<std::shared_ptr<Sensor>>, CborSensorsConfig>,
    public IConfigurationManager {
public:
    /// The CAN bus configuration the unit runs; nullptr is one without channels.
    using CanbusConfigurationProvider = std::function<std::shared_ptr<const CanbusConfiguration>()>;

private:
    SensorsCborParser cbor_parser_;
    ConfigurationUpdatedHandler configuration_updated_handler_;
    CanbusConfigurationProvider canbus_configuration_provider_;

    int gpio_channel_count_;
    int adc_channel_count_;

    eerie_memory::PmrUniquePtr<CborSensorsConfig>
    Serialize(const std::vector<std::shared_ptr<Sensor>>& sensors) override;
    eerie_memory::PmrUniquePtr<std::vector<std::shared_ptr<Sensor>>>
    Deserialize(const CborSensorsConfig& cbor_config) override;
    bool CreateDefaultConfiguration() override;
    void ValidateApply(const std::vector<std::shared_ptr<Sensor>>& sensors) override;

public:
    /**
     * @param canbus_configuration_provider The CAN bus configuration a client's CAN sensors must name
     *        a configured bus, frame and signal of; without it they are not checked against one.
     */
    SensorsConfigurationManager(
        std::unique_ptr<config_service::CborConfigurationService<CborSensorsConfig>> cbor_configuration_service,
        std::shared_ptr<IFsService> sd_fs_service,
        int gpio_channel_count,
        int adc_channel_count,
        CanbusConfigurationProvider canbus_configuration_provider = nullptr
    );

    // Called after a configuration has been stored through Update() or ApplyCborConfiguration(), not on load.
    void RegisterConfigurationUpdatedHandler(ConfigurationUpdatedHandler handler) override;

    using CborConfigurationManagerBase::ApplyCborConfiguration;

    bool Update(const std::vector<std::shared_ptr<Sensor>>& sensors) override;
    bool ApplyCborConfiguration(std::span<const uint8_t> cbor_data, std::span<char> reason) override;

    [[nodiscard]] int GetGpioChannelCount() const {
        return gpio_channel_count_;
    }
    [[nodiscard]] int GetAdcChannelCount() const {
        return adc_channel_count_;
    }
};

} // namespace eerie_leap::domain::sensor_domain::configuration
