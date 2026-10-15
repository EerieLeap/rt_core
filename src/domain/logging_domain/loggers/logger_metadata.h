#pragma once

#include <cstdint>
#include <string>

namespace eerie_leap::domain::logging_domain::loggers {

// Who wrote the log, written into the file header. LogWriterSupervisor fills it at every start: the
// tool ID and the CDMP device ID from its owner, the rest from Kconfig and SystemConfiguration, whose
// packed version layout it uses.
struct LoggerMetadata {
    std::string device_name;
    std::string tool_id;
    std::string tool_version;

    uint32_t product_id = 0;
    uint8_t device_id = 0;
    uint32_t hw_version = 0;
    uint32_t sw_version = 0;
    uint32_t sensors_configuration_hash = 0;   // 0 when unknown
};

} // namespace eerie_leap::domain::logging_domain::loggers
