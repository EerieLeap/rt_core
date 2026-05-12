#pragma once

#include <zephyr/data/json.h>
#include <eerie_memory.hpp>

#include "configuration/json/configs/json_logging_config.h"
#include "domain/logging_domain/models/logging_configuration.h"

namespace eerie_leap::domain::logging_domain::configuration::parsers {

using eerie_leap::configuration::json::configs::JsonLoggingConfig;
using eerie_leap::domain::logging_domain::models::LoggingConfiguration;

class LoggingConfigurationJsonParser {
public:
    LoggingConfigurationJsonParser() = default;

    eerie_memory::pmr_unique_ptr<JsonLoggingConfig> Serialize(const LoggingConfiguration& configuration);
    eerie_memory::pmr_unique_ptr<LoggingConfiguration> Deserialize(std::pmr::memory_resource* mr, const JsonLoggingConfig& config);
};

} // namespace eerie_leap::domain::logging_domain::configuration::parsers
