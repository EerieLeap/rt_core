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

} // namespace eerie_leap::domain::logging_domain::event_bus
