#pragma once

#include "subsys/event_bus/event_binding.h"

#include "domain/logging_domain/control/models/logging_control_configuration.h"

namespace eerie_leap::domain::logging_domain::control::configuration::parsers {

using eerie_leap::domain::logging_domain::control::models::LoggingControlConfiguration;
using eerie_leap::subsys::event_bus::EventBindingChecker;

// Validate() makes structural checks only, so it runs on load, save and apply: whether a channel is
// registered on this unit, or a sensor exists, is not known to it, and a trigger that names neither
// does nothing when it binds. ValidateBindings() asks the unit, through a checker the caller
// injects, and runs only when a client applies a configuration. The class itself needs neither the
// event bus nor its registry.
class LoggingControlConfigurationValidator {
public:
    // Throws std::invalid_argument, naming the first trigger that fails.
    static void Validate(const LoggingControlConfiguration& configuration);

    // Throws std::invalid_argument for the first trigger @p check_binding gives a reason for (an
    // event type or payload key its channel does not have, a channel this unit does not carry).
    static void
    ValidateBindings(const LoggingControlConfiguration& configuration, const EventBindingChecker& check_binding);
};

} // namespace eerie_leap::domain::logging_domain::control::configuration::parsers
