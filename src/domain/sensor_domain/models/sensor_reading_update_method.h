#pragma once

#include <cstdint>

namespace eerie_leap::domain::sensor_domain::models {

enum class SensorReadingUpdateMethod : uint8_t {
    NONE,
    ISR,       ///< Updated by its source: a CAN frame, a GPIO edge or a control's value.
    SCHEDULER, ///< Polled at its sampling rate.
    DEPENDENT, ///< Evaluated right after one of the sensors its expression reads commits.
};

} // namespace eerie_leap::domain::sensor_domain::models

// namespace eerie_leap::domain::sensor_domain::models
