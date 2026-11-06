#pragma once

#include <chrono>
#include <optional>

namespace eerie_leap::domain::logging_domain::loggers {

/** What a logger needs to know about the start of one file. */
struct LogFileStart {
    /** The file's start on the unit's clock: the origin of every record time. */
    std::chrono::system_clock::time_point time;
    /** Set: real time minus the unit's clock, and the file is on real time (header start = time + offset). */
    std::optional<std::chrono::system_clock::duration> wall_clock_offset;
};

} // namespace eerie_leap::domain::logging_domain::loggers
