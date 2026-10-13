#pragma once

#include <cstdint>
#include <string>

namespace eerie_leap::domain::logging_domain::loggers {

// Who wrote the log, supplied by the application so the logging domain stays independent of the
// system domain. Written into the file header; versions use the system domain's packed layout.
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
