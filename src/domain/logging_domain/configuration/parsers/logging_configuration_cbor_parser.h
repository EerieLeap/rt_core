#pragma once

#include <eerie_memory.hpp>

#include "configuration/cbor/cbor_logging_config/cbor_logging_config.h"
#include "domain/logging_domain/models/logging_configuration.h"

namespace eerie_leap::domain::logging_domain::configuration::parsers {

using eerie_leap::domain::logging_domain::models::LoggingConfiguration;

class LoggingConfigurationCborParser {
public:
    LoggingConfigurationCborParser() = default;

    static eerie_memory::PmrUniquePtr<CborLoggingConfig> Serialize(const LoggingConfiguration& logging_configuration);
    static eerie_memory::PmrUniquePtr<LoggingConfiguration>
    Deserialize(std::pmr::memory_resource* mr, const CborLoggingConfig& logging_config);
};

} // namespace eerie_leap::domain::logging_domain::configuration::parsers
