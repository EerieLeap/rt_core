#pragma once

#include <memory>
#include <span>

#include <zephyr/kernel.h>

#include "configuration/cbor/cbor_logging_config/cbor_logging_config.h"
#include "configuration/services/cbor_configuration_service.h"
#include "utilities/memory/memory_resource_manager.h"

#include "domain/configuration_domain/utilities/cbor_configuration_manager_base.h"
#include "domain/configuration_domain/utilities/i_configuration_manager.h"
#include "domain/logging_domain/configuration/parsers/logging_configuration_cbor_parser.h"
#include "domain/logging_domain/models/logging_configuration.h"

namespace eerie_leap::domain::logging_domain::configuration {

namespace config_service = eerie_leap::configuration::services;

using eerie_leap::domain::configuration_domain::utilities::CborConfigurationManagerBase;
using eerie_leap::domain::configuration_domain::utilities::IConfigurationManager;
using eerie_leap::domain::logging_domain::configuration::parsers::LoggingConfigurationCborParser;
using eerie_leap::domain::logging_domain::models::LoggingConfiguration;

class LoggingConfigurationManager :
    public CborConfigurationManagerBase<LoggingConfiguration, CborLoggingConfig>,
    public IConfigurationManager {
private:
    LoggingConfigurationCborParser cbor_parser_;

    // The writer's supervisor clears the handler when it goes, while a store on another thread may
    // be calling it. A store copies the pointer under the lock and calls it outside, so a cleared
    // handler finishes the call it started on its own reference, and a call allocates nothing.
    k_mutex handler_mutex_{};
    std::shared_ptr<const ConfigurationUpdatedHandler> configuration_updated_handler_;

    void NotifyUpdated();

    eerie_memory::pmr_unique_ptr<CborLoggingConfig> Serialize(const LoggingConfiguration& configuration) override;
    eerie_memory::pmr_unique_ptr<LoggingConfiguration> Deserialize(const CborLoggingConfig& cbor_config) override;
    bool CreateDefaultConfiguration() override;

public:
    explicit LoggingConfigurationManager(
        std::unique_ptr<config_service::CborConfigurationService<CborLoggingConfig>> cbor_configuration_service
    );

    // Called after a configuration has been stored through Update() or ApplyCborConfiguration(), not
    // on load, on the thread that stored it. Callable from any thread; nullptr clears it.
    void RegisterConfigurationUpdatedHandler(ConfigurationUpdatedHandler handler) override;

    using CborConfigurationManagerBase::ApplyCborConfiguration;

    bool Update(const LoggingConfiguration& configuration);
    bool ApplyCborConfiguration(std::span<const uint8_t> cbor_data, std::span<char> reason) override;
};

} // namespace eerie_leap::domain::logging_domain::configuration
