#pragma once

#include <cstdint>
#include <chrono>

namespace eerie_leap::domain::logging_domain::loggers {

template<typename T>
class ILogger {
public:
    virtual ~ILogger() = default;

    virtual const char* GetFileExtension() const = 0;
    virtual bool StartLogging(std::streambuf& stream, const std::chrono::system_clock::time_point& start_time) = 0;
    virtual bool StopLogging() = 0;
    virtual bool LogReading(const std::chrono::system_clock::time_point& time, const T& reading) = 0;
};

} // namespace eerie_leap::domain::logging_domain::loggers
