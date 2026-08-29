#pragma once

#include <cstdint>

namespace eerie_leap::domain::logging_domain::event_bus {

enum class LoggingEventType : std::uint32_t {
    StatusUpdated
};

} // namespace eerie_leap::domain::logging_domain::event_bus
