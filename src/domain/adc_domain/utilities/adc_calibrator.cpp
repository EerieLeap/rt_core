#include <algorithm>
#include <stdexcept>

#include "adc_calibrator.h"
#include "utilities/voltage_interpolator/cubic_spline_voltage_interpolator.hpp"
#include "utilities/voltage_interpolator/linear_voltage_interpolator.hpp"

namespace eerie_leap::domain::adc_domain::utilities {

using namespace eerie_leap::utilities::voltage_interpolator;

AdcCalibrator::AdcCalibrator(
    InterpolationMethod interpolation_method,
    std::shared_ptr<std::pmr::vector<CalibrationData>> calibration_data,
    AdcRange range
) :
    range_(range),
    calibration_data_(std::move(calibration_data)) {

    if(calibration_data_ == nullptr)
        throw std::invalid_argument("Calibration data is missing or invalid.");

    if(!(range_.adc_max_v > 0.0F) || !(range_.sensor_max_v > 0.0F))
        throw std::invalid_argument("ADC range must be positive.");

    // The interpolators map a reading to the true voltage, so the measured column becomes the key,
    // in ADC-pin volts.
    auto normalized_data = std::make_shared<std::pmr::vector<CalibrationData>>(calibration_data_->get_allocator());
    normalized_data->reserve(calibration_data_->size());

    for(const auto& point : *calibration_data_) {
        normalized_data->push_back({ .voltage = range_.ToAdcVolts(point.value), .value = point.voltage });
    }

    // Both interpolators binary search their key, and a table ordered by its true voltages is
    // ordered by its readings only when the readings are monotonic.
    std::ranges::sort(*normalized_data, {}, &CalibrationData::voltage);

    switch(interpolation_method) {
        case InterpolationMethod::LINEAR:
            calibrated_voltage_interpolator_ = std::make_unique<LinearVoltageInterpolator>(std::move(normalized_data));
            break;

        case InterpolationMethod::CUBIC_SPLINE:
            calibrated_voltage_interpolator_ = std::make_unique<CubicSplineVoltageInterpolator>(
                std::move(normalized_data)
            );
            break;

        default:
            throw std::runtime_error("Interpolation method is not supported.");
    }
}

float AdcCalibrator::InterpolateToInputRange(float value) const {
    return range_.ToSensorVolts(value);
}

// Clamped like ToSensorVolts, so a reading beyond the ADC range lands on the key the calibration
// recorded for it rather than being extrapolated past the table.
float AdcCalibrator::InterpolateToCalibratedRange(float value) const {
    return calibrated_voltage_interpolator_->Interpolate(std::clamp(value, 0.0F, range_.adc_max_v));
}

InterpolationMethod AdcCalibrator::GetInterpolationMethod() const {
    return calibrated_voltage_interpolator_->GetInterpolationMethod();
}

std::shared_ptr<std::pmr::vector<CalibrationData>> AdcCalibrator::GetCalibrationTable() const {
    return calibration_data_;
}

} // namespace eerie_leap::domain::adc_domain::utilities
