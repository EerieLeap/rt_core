#pragma once

#include "domain/logging_domain/control/models/logging_control_configuration.h"

namespace eerie_leap::domain::logging_domain::control::configuration::parsers {

using eerie_leap::domain::logging_domain::control::models::LoggingControlConfiguration;

// Structural checks only: whether a channel is registered on this unit, or a sensor exists, is not
// known here, and a trigger that names neither does nothing when it binds.
class LoggingControlConfigurationValidator {
public:
    // Throws std::invalid_argument, naming the first trigger that fails.
    static void Validate(const LoggingControlConfiguration& configuration);
};

} // namespace eerie_leap::domain::logging_domain::control::configuration::parsers
