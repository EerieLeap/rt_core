#pragma once

#include <atomic>
#include <cstdint>

#include "logging_start_cause.h"

namespace eerie_leap::domain::logging_domain::models {

/**
 * What the files written for one start of logging share: LogWriterSupervisor makes one when it
 * starts a writer that was not running, and every logger of that start (its rotations, and the
 * restarts a stored configuration causes) is given it. The loggers write the ID into each header and
 * number the files. Not to be confused with LogSessionInfo, which is the status's view of the writer.
 */
struct LogSession {
    uint32_t id = 0; // Never 0 for a supervisor's session; 0 in a header means no session.

    // What asked for the start (F-9); fixed before the session is shared.
    LoggingStartCause cause = LoggingStartCause::Unknown;

    // The index the next file of the session takes. A logger moves it on once its header has synced;
    // loggers of one session run one at a time, and the atomic keeps a reader off a torn value.
    std::atomic<uint32_t> next_file_index{ 0 };
};

} // namespace eerie_leap::domain::logging_domain::models
