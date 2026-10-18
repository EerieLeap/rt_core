#pragma once

#include <cstdint>

namespace eerie_leap::domain::sensor_domain::models {

enum class ReadingSource : uint8_t {
    NONE,
    ISR,
    PROCESSING,
};

} // namespace eerie_leap::domain::sensor_domain::models

// namespace eerie_leap::domain::sensor_domain::models
