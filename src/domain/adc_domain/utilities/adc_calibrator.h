#pragma once

#include <memory>
#include <memory_resource>
#include <vector>

#include "utilities/voltage_interpolator/calibration_data.h"
#include "utilities/voltage_interpolator/i_voltage_interpolator.h"

#include "domain/adc_domain/models/adc_range.h"

namespace eerie_leap::domain::adc_domain::utilities {

using eerie_leap::domain::adc_domain::models::AdcRange;
using eerie_leap::utilities::voltage_interpolator::CalibrationData;
using eerie_leap::utilities::voltage_interpolator::InterpolationMethod;
using eerie_leap::utilities::voltage_interpolator::IVoltageInterpolator;

// The calibration table is kept as measured: voltage is the true voltage at the sensor input,
// value is what the unit read for it, in sensor volts (AdcRange::ToSensorVolts of the ADC reading).
class AdcCalibrator {
private:
    AdcRange range_;
    std::shared_ptr<std::pmr::vector<CalibrationData>> calibration_data_ = nullptr;
    std::unique_ptr<IVoltageInterpolator> calibrated_voltage_interpolator_ = nullptr;

public:
    AdcCalibrator(
        InterpolationMethod interpolation_method,
        std::shared_ptr<std::pmr::vector<CalibrationData>> calibration_data,
        AdcRange range = AdcRange::Default()
    );

    // ADC-pin volts to sensor volts through the range alone.
    [[nodiscard]] float InterpolateToInputRange(float value) const;
    // ADC-pin volts, clamped to the ADC range, to calibrated sensor volts.
    [[nodiscard]] float InterpolateToCalibratedRange(float value) const;

    [[nodiscard]] InterpolationMethod GetInterpolationMethod() const;
    [[nodiscard]] std::shared_ptr<std::pmr::vector<CalibrationData>> GetCalibrationTable() const;
    [[nodiscard]] const AdcRange& GetRange() const {
        return range_;
    }
};

} // namespace eerie_leap::domain::adc_domain::utilities
