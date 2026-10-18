#pragma once

#include <memory_resource>

#include <eerie_memory.hpp>

#include "configuration/cbor/cbor_logging_control_config/cbor_logging_control_config.h"
#include "domain/logging_domain/control/models/logging_control_configuration.h"

namespace eerie_leap::domain::logging_domain::control::configuration::parsers {

using eerie_leap::domain::logging_domain::control::models::LoggingControlConfiguration;

class LoggingControlConfigurationCborParser {
public:
    LoggingControlConfigurationCborParser() = default;

    // Validates first. A text selector becomes a view into @p configuration, which the caller keeps
    // alive until the result is encoded.
    static eerie_memory::PmrUniquePtr<CborLoggingControlConfig>
    Serialize(const LoggingControlConfiguration& configuration);
    // Copies what it keeps, so the result does not refer to @p config, and validates the result.
    static eerie_memory::PmrUniquePtr<LoggingControlConfiguration>
    Deserialize(std::pmr::memory_resource* mr, const CborLoggingControlConfig& config);
};

} // namespace eerie_leap::domain::logging_domain::control::configuration::parsers
