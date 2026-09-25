#pragma once

#include <memory>
#include <memory_resource>
#include <span>

#include "utilities/memory/memory_resource_manager.h"
#include "configuration/cbor/cbor_canbus_config/cbor_canbus_config.h"
#include "configuration/services/cbor_configuration_service.h"

#include "subsys/fs/services/i_fs_service.h"

#include "domain/configuration_domain/utilities/i_configuration_manager.h"
#include "domain/configuration_domain/utilities/cbor_configuration_manager_base.h"
#include "domain/canbus_domain/configuration/parsers/canbus_configuration_cbor_parser.h"
#include "domain/canbus_domain/models/canbus_configuration.h"

namespace eerie_leap::domain::canbus_domain::configuration {

namespace config_service = eerie_leap::configuration::services;

using eerie_leap::domain::configuration_domain::utilities::IConfigurationManager;
using eerie_leap::domain::configuration_domain::utilities::CborConfigurationManagerBase;
using eerie_leap::domain::canbus_domain::configuration::parsers::CanbusConfigurationCborParser;
using eerie_leap::domain::canbus_domain::models::CanbusConfiguration;
using eerie_leap::subsys::fs::services::IFsService;

class CanbusConfigurationManager
    : public CborConfigurationManagerBase<CanbusConfiguration, CborCanbusConfig>,
      public IConfigurationManager {
private:
    CanbusConfigurationCborParser cbor_parser_;
    ConfigurationUpdatedHandler configuration_updated_handler_;

    eerie_memory::pmr_unique_ptr<CborCanbusConfig> Serialize(const CanbusConfiguration& configuration) override;
    eerie_memory::pmr_unique_ptr<CanbusConfiguration> Deserialize(const CborCanbusConfig& cbor_config) override;
    bool CreateDefaultConfiguration() override;

public:
    explicit CanbusConfigurationManager(
        std::unique_ptr<config_service::CborConfigurationService<CborCanbusConfig>> cbor_configuration_service,
        std::shared_ptr<IFsService> sd_fs_service);

    void RegisterConfigurationUpdatedHandler(ConfigurationUpdatedHandler handler) override;

    bool Update(const CanbusConfiguration& configuration);
    bool ApplyCborConfiguration(std::span<const uint8_t> cbor_data) override;
};

} // namespace eerie_leap::domain::canbus_domain::configuration
