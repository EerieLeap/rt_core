#pragma once

#include <algorithm>

namespace eerie_leap::domain::adc_domain::models {

// The input divider in front of the ADC: adc_max_v at the ADC pin is sensor_max_v at the sensor
// input. Voltages beyond either end saturate there; no calibration is applied.
struct AdcRange {
    float adc_max_v = 0.0F;
    float sensor_max_v = 0.0F;

    [[nodiscard]] static constexpr AdcRange Default() {
        return {
            .adc_max_v = CONFIG_EERIE_LEAP_ADC_VOLTAGE_MAX_MV / 1000.0F,
            .sensor_max_v = CONFIG_EERIE_LEAP_SENSOR_VOLTAGE_MAX_MV / 1000.0F
        };
    }

    [[nodiscard]] constexpr float ToSensorVolts(float adc_v) const noexcept {
        return std::clamp(adc_v, 0.0F, adc_max_v) * (sensor_max_v / adc_max_v);
    }

    [[nodiscard]] constexpr float ToAdcVolts(float sensor_v) const noexcept {
        return std::clamp(sensor_v, 0.0F, sensor_max_v) * (adc_max_v / sensor_max_v);
    }
};

} // namespace eerie_leap::domain::adc_domain::models
