#pragma once

#include <cstdint>

namespace eerie_leap::domain::system_domain::models {

/** @brief The sensors a unit can have, as its sensor configuration is validated against them. */
struct SensorCapabilities {
    uint32_t adc_channels = 0;   ///< ADC channels, numbered from 0, a PHYSICAL_ANALOG sensor can read.
    uint32_t gpio_channels = 0;  ///< GPIO channels, numbered from 0, a PHYSICAL_INDICATOR sensor can read.
    uint32_t sensors_max = 0;    ///< Sensors a configuration can hold.
};

} // namespace eerie_leap::domain::system_domain::models
