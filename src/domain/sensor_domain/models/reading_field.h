#pragma once

#include <cstdint>

namespace eerie_leap::domain::sensor_domain::models {

// A number a reading carries, picked by the clients of the live data. On the wire: append only.
enum class ReadingField : uint8_t {
    VALUE = 0,              ///< The processed value.
    CALIBRATED_VOLTAGE = 1, ///< ADC voltage after the ADC calibration, PHYSICAL_ANALOG only.
    ADC_VOLTAGE = 2,        ///< ADC voltage before calibration, PHYSICAL_ANALOG only.
};

} // namespace eerie_leap::domain::sensor_domain::models
