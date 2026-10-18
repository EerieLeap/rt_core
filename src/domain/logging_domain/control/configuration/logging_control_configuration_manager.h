#pragma once

#include <memory>
#include <span>

#include <zephyr/kernel.h>

#include "configuration/cbor/cbor_logging_control_config/cbor_logging_control_config.h"
#include "configuration/services/cbor_configuration_service.h"
#include "utilities/memory/memory_resource_manager.h"

#include "domain/configuration_domain/utilities/cbor_configuration_manager_base.h"
#include "domain/configuration_domain/utilities/i_configuration_manager.h"
#include "domain/logging_domain/control/configuration/parsers/logging_control_configuration_cbor_parser.h"
#include "domain/logging_domain/control/models/logging_control_configuration.h"

namespace eerie_leap::domain::logging_domain::control::configuration {

namespace config_service = eerie_leap::configuration::services;

using eerie_leap::domain::configuration_domain::utilities::CborConfigurationManagerBase;
using eerie_leap::domain::configuration_domain::utilities::IConfigurationManager;
using eerie_leap::domain::logging_domain::control::configuration::parsers::LoggingControlConfigurationCborParser;
using eerie_leap::domain::logging_domain::control::models::LoggingControlConfiguration;

// The stored logging triggers (ConfigurationService::Type::LoggingControl). The controller creates
// it with the file name "logging_control_config" and registers it; the default is no triggers.
class LoggingControlConfigurationManager :
    public CborConfigurationManagerBase<LoggingControlConfiguration, CborLoggingControlConfig>,
    public IConfigurationManager {
private:
    LoggingControlConfigurationCborParser cbor_parser_;

    // The controller sets and clears the handler while a store on another thread may be calling it.
    // A store copies the pointer under the lock and calls it outside, so a cleared handler finishes
    // the call it started on its own reference, and a call allocates nothing.
    k_mutex handler_mutex_{};
    std::shared_ptr<const ConfigurationUpdatedHandler> configuration_updated_handler_;

    // Logs what the handler throws: the configuration is stored by then.
    void NotifyUpdated();

    eerie_memory::PmrUniquePtr<CborLoggingControlConfig>
    Serialize(const LoggingControlConfiguration& configuration) override;
    eerie_memory::PmrUniquePtr<LoggingControlConfiguration>
    Deserialize(const CborLoggingControlConfig& cbor_config) override;
    bool CreateDefaultConfiguration() override;

public:
    explicit LoggingControlConfigurationManager(
        std::unique_ptr<config_service::CborConfigurationService<CborLoggingControlConfig>> cbor_configuration_service
    );

    // Called after a configuration has been stored through Update() or ApplyCborConfiguration(), not
    // on load, on the thread that stored it (an SMP apply: the configuration work queue). Callable
    // from any thread; nullptr clears it. An exception it throws is logged and does not fail the
    // store, which has happened by then.
    void RegisterConfigurationUpdatedHandler(ConfigurationUpdatedHandler handler) override;

    using CborConfigurationManagerBase::ApplyCborConfiguration;

    bool Update(const LoggingControlConfiguration& configuration);
    bool ApplyCborConfiguration(std::span<const uint8_t> cbor_data, std::span<char> reason) override;
};

} // namespace eerie_leap::domain::logging_domain::control::configuration
