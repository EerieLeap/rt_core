#pragma once

#include <chrono>
#include <cstdint>
#include <streambuf>

#include "log_file_end.h"
#include "log_file_start.h"

namespace eerie_leap::domain::logging_domain::loggers {

template<typename T>
class ILogger {
public:
    virtual ~ILogger() = default;

    virtual const char* GetFileExtension() const = 0;
    virtual bool StartLogging(std::streambuf& stream, const LogFileStart& start) = 0;
    /**
     * Finalizes the file and, when that worked, appends the event that says how it ended. False when
     * the file could not be finalized; never throws, so a stop always closes the file. A failed
     * event does not fail the stop.
     */
    virtual bool StopLogging(const LogFileEnd& end) = 0;
    /**
     * Writes @p reading at its own timestamp. @p time is the caller's current time (the tick): it
     * stands in for a reading without a timestamp, and it is when a sample already written is
     * written again, for a sensor that logs every tick. False when the reading is not logged.
     * Throws std::ios_base::failure, and nothing else, when the stream refuses the record.
     */
    virtual bool LogReading(const std::chrono::system_clock::time_point& time, const T& reading) = 0;
    /** Bytes written to the stream since StartLogging(), header included. */
    [[nodiscard]] virtual uint64_t GetBytesWritten() const = 0;
};

} // namespace eerie_leap::domain::logging_domain::loggers
