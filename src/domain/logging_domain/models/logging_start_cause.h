#pragma once

#include <cstdint>
#include <string_view>

namespace eerie_leap::domain::logging_domain::models {

// What asked for a start of logging. Written by name into the log's header (start_cause). Append only.
enum class LoggingStartCause : uint8_t {
    Unknown,
    Trigger, // A logging trigger.
    Command, // The CDMP LOGGING command of another unit.
    Smp,     // The logging group's START: the app, or the bench client.
    Shell,
    Autostart
};

// No default case, so a new value warns here.
constexpr std::string_view GetStartCauseName(LoggingStartCause cause) {
    switch(cause) {
        case LoggingStartCause::Unknown:
            return "unknown";
        case LoggingStartCause::Trigger:
            return "trigger";
        case LoggingStartCause::Command:
            return "command";
        case LoggingStartCause::Smp:
            return "smp";
        case LoggingStartCause::Shell:
            return "shell";
        case LoggingStartCause::Autostart:
            return "autostart";
    }

    return "unknown";
}

} // namespace eerie_leap::domain::logging_domain::models
