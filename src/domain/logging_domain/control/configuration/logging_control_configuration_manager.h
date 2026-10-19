#pragma once

#include <memory>
#include <span>

#include <zephyr/kernel.h>

#include "configuration/cbor/cbor_logging_control_config/cbor_logging_control_config.h"
#include "configuration/services/cbor_configuration_service.h"
#include "utilities/memory/memory_resource_manager.h"

#include "subsys/event_bus/event_binding.h"

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
using eerie_leap::subsys::event_bus::EventBindingChecker;

// The stored logging triggers (ConfigurationService::Type::LoggingControl). The controller creates
// it with the file name "logging_control_config" and registers it; the default is no triggers.
class LoggingControlConfigurationManager :
    public CborConfigurationManagerBase<LoggingControlConfiguration, CborLoggingControlConfig>,
    public IConfigurationManager {
private:
    // The controller sets and clears the handler while a store on another thread may be calling it.
    // A store copies the pointer under the lock and calls it outside, so a cleared handler finishes
    // the call it started on its own reference, and a call allocates nothing.
    k_mutex handler_mutex_{};
    std::shared_ptr<const ConfigurationUpdatedHandler> configuration_updated_handler_;

    EventBindingChecker check_binding_;

    // Logs what the handler throws: the configuration is stored by then.
    void NotifyUpdated();

    eerie_memory::PmrUniquePtr<CborLoggingControlConfig>
    Serialize(const LoggingControlConfiguration& configuration) override;
    eerie_memory::PmrUniquePtr<LoggingControlConfiguration>
    Deserialize(const CborLoggingControlConfig& cbor_config) override;
    bool CreateDefaultConfiguration() override;

    // Refuses a client's trigger the unit cannot resolve; loading and Update() do not call it.
    void ValidateApply(const LoggingControlConfiguration& configuration) override;

public:
    /**
     * @param check_binding Tells why this unit could not resolve a trigger's binding (the
     *        controller passes GetEventBindingError). A client's apply is refused for the first
     *        trigger it rejects; without one the bindings are not checked against the unit.
     */
    explicit LoggingControlConfigurationManager(
        std::unique_ptr<config_service::CborConfigurationService<CborLoggingControlConfig>> cbor_configuration_service,
        EventBindingChecker check_binding = {}
    );

    // Called after a configuration has been stored through Update() or ApplyCborConfiguration(), not
    // on load, on the thread that stored it (an SMP apply: the configuration work queue). Callable
    // from any thread; nullptr clears it. An exception it throws is logged and does not fail the
    // store, which has happened by then.
    void RegisterConfigurationUpdatedHandler(ConfigurationUpdatedHandler handler) override;

    using CborConfigurationManagerBase::ApplyCborConfiguration;

    bool Update(const LoggingControlConfiguration& configuration) override;
    bool ApplyCborConfiguration(std::span<const uint8_t> cbor_data, std::span<char> reason) override;
};

} // namespace eerie_leap::domain::logging_domain::control::configuration
