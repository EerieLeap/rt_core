#pragma once

#include "domain/logging_domain/models/logging_configuration.h"

namespace eerie_leap::domain::logging_domain::configuration::parsers {

using eerie_leap::domain::logging_domain::models::LoggingConfiguration;

class LoggingConfigurationValidator {
private:
    static void ValidateLoggingIntervalMs(const LoggingConfiguration& configuration);
    static void ValidateMaxLogSizeMb(const LoggingConfiguration& configuration);
    static void ValidateSensorConfigurations(const LoggingConfiguration& configuration);

public:
    static void Validate(const LoggingConfiguration& configuration);
};

} // namespace eerie_leap::domain::logging_domain::configuration::parsers
