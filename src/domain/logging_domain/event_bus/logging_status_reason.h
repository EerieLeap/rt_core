#pragma once

#include <cstdint>

namespace eerie_leap::domain::logging_domain::event_bus {

// Why logging became inactive: the LoggingPayloadType::Reason value of a StatusUpdated event with
// IsActive = false. Add values, never rename or renumber them: the logging status record
// (CanbusComLoggingState) carries them to other units. The record includes this header, and so does
// every logging event header, so it stays free of CAN includes.
enum class LoggingStatusReason : std::uint32_t {
    Stopped,          // Stop() finalized and closed the file.
    WriteFailed,      // A record or a sync did not reach the card.
    NoSpace,          // Less than EERIE_LEAP_LOG_MIN_FREE_SPACE_MB was free at a rotation.
    RotationFailed,   // The next file of a rotation could not be opened or its header written.
    Unavailable       // Only imported: no unit reports a status (no logger is online, or this unit is not).
};

// The reason as log and shell text. No default case, so a new value is a -Wswitch warning until it
// is named here; "unknown" is for a value this build does not know, which a newer unit's record
// can carry.
constexpr const char* GetReasonName(LoggingStatusReason reason) {
    switch(reason) {
        case LoggingStatusReason::Stopped:
            return "stopped";
        case LoggingStatusReason::WriteFailed:
            return "write failed";
        case LoggingStatusReason::NoSpace:
            return "no space";
        case LoggingStatusReason::RotationFailed:
            return "rotation failed";
        case LoggingStatusReason::Unavailable:
            return "unavailable";
    }

    return "unknown";
}

} // namespace eerie_leap::domain::logging_domain::event_bus
