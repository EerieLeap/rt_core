#pragma once

#include <cstdint>

namespace eerie_leap::domain::logging_domain::event_bus {

enum class LoggingEventType : std::uint32_t {
    StatusUpdated
};

// The channel bounds HasEventType() with it; appending a value means moving this to it.
inline constexpr LoggingEventType LAST_LOGGING_EVENT_TYPE = LoggingEventType::StatusUpdated;

} // namespace eerie_leap::domain::logging_domain::event_bus
