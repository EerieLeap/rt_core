#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <chrono>
#include <string_view>

#include "utilities/string/static_string.hpp"

namespace eerie_leap::domain::logging_domain::services {

using eerie_leap::utilities::string::StaticString;

class LogFileNaming {
public:
    /// The directory of the log files, relative to the log volume; also the directory of the LOGS
    /// assets.
    static constexpr const char* LOGS_DIR = "logs";
    static constexpr size_t PATH_MAX_LENGTH = 128;
    static constexpr uint32_t RANDOM_SUFFIX_MODULO = 10000;

    /**
     * '<directory>/<prefix>_<YYYYMMDD>T<HHMMSS>Z_<rand4>.<extension>' in UTC, so names sort by start
     * time; the prefix and its underscore are left out when it is empty. @p random_value is reduced
     * to four decimal digits; it only separates files started within the same second.
     */
    static StaticString<PATH_MAX_LENGTH> GetFilePath(
        std::string_view directory,
        std::string_view prefix,
        const std::chrono::system_clock::time_point& time,
        uint32_t random_value,
        std::string_view extension) {

        const std::time_t time_s = std::chrono::system_clock::to_time_t(time);
        std::tm time_utc{};
        gmtime_r(&time_s, &time_utc);

        // Sized for any int in every field, which the format truncation check assumes.
        char stamp[80];
        snprintf(stamp, sizeof(stamp), "%04d%02d%02dT%02d%02d%02dZ_%04u",
            time_utc.tm_year + 1900, time_utc.tm_mon + 1, time_utc.tm_mday,
            time_utc.tm_hour, time_utc.tm_min, time_utc.tm_sec,
            static_cast<unsigned int>(random_value % RANDOM_SUFFIX_MODULO));

        StaticString<PATH_MAX_LENGTH> path;

        path += directory;
        path += "/";
        if(!prefix.empty()) {
            path += prefix;
            path += "_";
        }
        path += stamp;
        path += ".";
        path += extension;

        return path;
    }
};

} // namespace eerie_leap::domain::logging_domain::services
