#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

#include "domain/logging_domain/event_bus/logging_status_reason.h"

namespace eerie_leap::domain::logging_domain::loggers {

/** What a logger needs to know about the end of one file. */
struct LogFileEnd {
    /** The file's last tick on the unit's clock (its start when it had none): the origin of the end event's time. */
    std::chrono::system_clock::time_point time;
    /** Why the session ended; empty when the file was closed for a rotation and the session goes on. */
    std::optional<eerie_leap::domain::logging_domain::event_bus::LoggingStatusReason> stop_reason;
    /** Deadlines skipped and raw CAN frames dropped while this file was open. */
    uint32_t overruns = 0;
    uint32_t frames_dropped = 0;
};

} // namespace eerie_leap::domain::logging_domain::loggers
