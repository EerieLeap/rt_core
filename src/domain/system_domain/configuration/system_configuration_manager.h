#pragma once

#include <memory>

#include "configuration/cbor/cbor_system_config/cbor_system_config.h"
#include "configuration/services/cbor_configuration_service.h"
#include "domain/configuration_domain/utilities/cbor_configuration_manager_base.h"
#include "domain/system_domain/configuration/parsers/system_configuration_cbor_parser.h"
#include "domain/system_domain/models/system_configuration.h"

namespace eerie_leap::domain::system_domain::configuration {

namespace config_service = eerie_leap::configuration::services;
using eerie_leap::domain::configuration_domain::utilities::CborConfigurationManagerBase;
using eerie_leap::domain::system_domain::configuration::parsers::SystemConfigurationCborParser;
using eerie_leap::domain::system_domain::models::SystemConfiguration;

class SystemConfigurationManager : public CborConfigurationManagerBase<SystemConfiguration, CborSystemConfig> {
private:
    SystemConfigurationCborParser cbor_parser_;

    eerie_memory::pmr_unique_ptr<CborSystemConfig> Serialize(const SystemConfiguration& configuration) override;
    eerie_memory::pmr_unique_ptr<SystemConfiguration> Deserialize(const CborSystemConfig& cbor_config) override;
    bool CreateDefaultConfiguration() override;

public:
    explicit SystemConfigurationManager(
        std::unique_ptr<config_service::CborConfigurationService<CborSystemConfig>> cbor_configuration_service);

    bool UpdateBuildNumber(uint32_t build_number);
};

} // namespace eerie_leap::domain::system_domain::configuration
