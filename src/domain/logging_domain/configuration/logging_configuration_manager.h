#pragma once

#include <memory>

#include "utilities/memory/memory_resource_manager.h"
#include "configuration/cbor/cbor_logging_config/cbor_logging_config.h"
#include "configuration/services/cbor_configuration_service.h"

#include "domain/configuration_domain/utilities/cbor_configuration_manager_base.h"
#include "domain/logging_domain/configuration/parsers/logging_configuration_cbor_parser.h"
#include "domain/logging_domain/models/logging_configuration.h"

namespace eerie_leap::domain::logging_domain::configuration {

namespace config_service = eerie_leap::configuration::services;

using eerie_leap::domain::configuration_domain::utilities::CborConfigurationManagerBase;
using eerie_leap::domain::logging_domain::configuration::parsers::LoggingConfigurationCborParser;
using eerie_leap::domain::logging_domain::models::LoggingConfiguration;

class LoggingConfigurationManager : public CborConfigurationManagerBase<LoggingConfiguration, CborLoggingConfig> {
private:
    LoggingConfigurationCborParser cbor_parser_;

    eerie_memory::pmr_unique_ptr<CborLoggingConfig> Serialize(const LoggingConfiguration& configuration) override;
    eerie_memory::pmr_unique_ptr<LoggingConfiguration> Deserialize(const CborLoggingConfig& cbor_config) override;
    bool CreateDefaultConfiguration() override;

public:
    explicit LoggingConfigurationManager(
        std::unique_ptr<config_service::CborConfigurationService<CborLoggingConfig>> cbor_configuration_service);
};

} // namespace eerie_leap::domain::logging_domain::configuration
