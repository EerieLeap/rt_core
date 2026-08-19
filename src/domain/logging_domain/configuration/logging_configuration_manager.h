#pragma once

#include <memory>

#include "utilities/memory/memory_resource_manager.h"
#include "configuration/cbor/cbor_logging_config/cbor_logging_config.h"
#include "configuration/services/cbor_configuration_service.h"

#include "domain/configuration_domain/utilities/i_cbor_configuration_manager.h"
#include "domain/logging_domain/configuration/parsers/logging_configuration_cbor_parser.h"
#include "domain/logging_domain/models/logging_configuration.h"

namespace eerie_leap::domain::logging_domain::configuration {

namespace config_service = eerie_leap::configuration::services;

using eerie_leap::domain::configuration_domain::utilities::ICborConfigurationManager;
using eerie_leap::domain::logging_domain::configuration::parsers::LoggingConfigurationCborParser;
using eerie_leap::domain::logging_domain::models::LoggingConfiguration;

class LoggingConfigurationManager : public ICborConfigurationManager {
private:
    std::unique_ptr<config_service::CborConfigurationService<CborLoggingConfig>> cbor_configuration_service_;

    std::unique_ptr<LoggingConfigurationCborParser> cbor_parser_;

    std::shared_ptr<LoggingConfiguration> configuration_;

    bool CreateDefaultConfiguration();

public:
    explicit LoggingConfigurationManager(
        std::unique_ptr<config_service::CborConfigurationService<CborLoggingConfig>> cbor_configuration_service);

    bool Update(const LoggingConfiguration& configuration);
    std::shared_ptr<LoggingConfiguration> Get(bool force_load = false);

    bool ApplyCborConfiguration(std::span<const uint8_t> cbor_data) override;
    std::pmr::vector<uint8_t> GetCborConfiguration() override;
};

} // namespace eerie_leap::domain::logging_domain::configuration
