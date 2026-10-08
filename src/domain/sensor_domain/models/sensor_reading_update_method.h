#pragma once

#include <cstdint>

namespace eerie_leap::domain::sensor_domain::models {

enum class SensorReadingUpdateMethod : uint8_t {
    NONE,
    ISR,         ///< Updated by its source: a CAN frame or a GPIO edge.
    SCHEDULER,   ///< Polled at its sampling rate.
    DEPENDENT,   ///< Evaluated right after one of the sensors its expression reads commits.
};

}

// namespace eerie_leap::domain::sensor_domain::models
