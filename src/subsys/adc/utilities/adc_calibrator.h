#pragma once

#include <memory_resource>
#include <vector>

#include "utilities/voltage_interpolator/calibration_data.h"
#include "utilities/voltage_interpolator/i_voltage_interpolator.h"

namespace eerie_leap::subsys::adc::utilities {

using eerie_leap::utilities::voltage_interpolator::CalibrationData;
using eerie_leap::utilities::voltage_interpolator::InterpolationMethod;
using eerie_leap::utilities::voltage_interpolator::IVoltageInterpolator;

class AdcCalibrator {
private:
    std::shared_ptr<std::pmr::vector<CalibrationData>> calibration_data_ = nullptr;
    std::unique_ptr<IVoltageInterpolator> calibrated_voltage_interpolator_ = nullptr;

public:
    AdcCalibrator(InterpolationMethod interpolation_method, const std::shared_ptr<std::pmr::vector<CalibrationData>> calibration_data);

    [[nodiscard]] static float InterpolateToInputRange(float value);

    [[nodiscard]] float InterpolateToCalibratedRange(float value);
    [[nodiscard]] InterpolationMethod GetInterpolationMethod() const;
    [[nodiscard]] std::shared_ptr<std::pmr::vector<CalibrationData>> GetCalibrationTable() const;
};

} // namespace eerie_leap::subsys::adc::utilities
