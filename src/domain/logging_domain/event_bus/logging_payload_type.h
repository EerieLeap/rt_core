#pragma once

#include <cstdint>

namespace eerie_leap::domain::logging_domain::event_bus {

enum class LoggingPayloadType : std::uint32_t {
    IsActive
};

} // namespace eerie_leap::domain::logging_domain::event_bus
