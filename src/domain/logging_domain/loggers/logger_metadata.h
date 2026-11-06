#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "domain/logging_domain/models/log_session.h"
#include "subsys/mdf/mdf_property.h"

namespace eerie_leap::domain::logging_domain::loggers {

// A file embedded in the log: a stored configuration, as the supervisor's provider hands it over.
struct LoggerAttachment {
    std::string file_name;
    std::string mime_type;
    std::string comment_text; // The text of the attachment's comment; the properties follow it
    std::vector<eerie_leap::subsys::mdf::MdfProperty> properties;
    std::vector<uint8_t> bytes;
};

// Who wrote the log, written into the file header. LogWriterSupervisor fills it at every start: the
// tool ID and the CDMP device ID and UID from its owner, the boot ID from the time subsystem, the
// session of the start, the rest from Kconfig and SystemConfiguration, whose packed version layout
// it uses. The attachments are the configurations the unit runs, embedded in every file.
struct LoggerMetadata {
    std::string device_name;
    std::string tool_id;
    std::string tool_version;

    uint32_t product_id = 0;
    uint8_t device_id = 0;
    uint32_t unit_uid = 0; // The CDMP UID, which stays when the device ID changes; 0 without a COM service
    uint32_t boot_id = 0;  // This power-on; 0 when unknown
    uint32_t hw_version = 0;
    uint32_t sw_version = 0;
    uint32_t sensors_configuration_hash = 0; // 0 when unknown

    // The start of logging this file belongs to; shared by all its files. Null: no session, session
    // ID 0 and file index 0.
    std::shared_ptr<models::LogSession> session;

    std::vector<LoggerAttachment> attachments;
};

} // namespace eerie_leap::domain::logging_domain::loggers
