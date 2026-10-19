#pragma once

#include <cstdint>

namespace eerie_leap::domain::logging_domain::event_bus {

// Values are part of the event contract shared with other applications: add keys, never rename
// or renumber them.
enum class LoggingPayloadType : std::uint32_t {
    IsActive, // bool
    Reason    // uint32_t, a LoggingStatusReason; sent with IsActive = false
};

// The channel bounds HasPayloadKey() with it; appending a value means moving this to it.
inline constexpr LoggingPayloadType LAST_LOGGING_PAYLOAD_TYPE = LoggingPayloadType::Reason;

} // namespace eerie_leap::domain::logging_domain::event_bus
