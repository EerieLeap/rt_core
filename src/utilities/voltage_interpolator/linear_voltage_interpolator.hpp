#pragma once

#include <algorithm>
#include <memory>
#include <vector>

#include "calibration_data.h"
#include "i_voltage_interpolator.h"
#include "interpolation_method.h"

namespace eerie_leap::utilities::voltage_interpolator {

class LinearVoltageInterpolator : public IVoltageInterpolator {
private:
    static const InterpolationMethod INTERPOLATION_METHOD = InterpolationMethod::LINEAR;
    std::shared_ptr<std::pmr::vector<CalibrationData>> calibration_table_;

public:
    explicit LinearVoltageInterpolator(std::shared_ptr<std::pmr::vector<CalibrationData>> calibration_table) :
        calibration_table_(std::move(calibration_table)) {

        if(!calibration_table_ || calibration_table_->size() < 2)
            throw std::invalid_argument("Calibration data is missing or invalid.");
    }

    float Interpolate(float voltage, bool clamp_to_ends = false) const override {
        const auto& table = *calibration_table_;

        if(clamp_to_ends) {
            if(voltage <= table.front().voltage)
                return table.front().value;
            if(voltage >= table.back().voltage)
                return table.back().value;
        }

        // Binary search for the correct interval
        auto upper = std::ranges::lower_bound(table, , voltage, [](const CalibrationData& data, float v) {
            return data.voltage < v;
        });

        // Keep the segment inside the table; out-of-range voltages extrapolate along the nearest one.
        if(upper == table.begin())
            ++upper;
        else if(upper == table.end())
            --upper;

        const auto lower = upper - 1;

        const float x0 = lower->voltage;
        const float x1 = upper->voltage;
        const float y0 = lower->value;
        const float y1 = upper->value;

        float ratio = 0.0F;
        if(x1 != x0)
            ratio = (voltage - x0) / (x1 - x0);

        return y0 + ratio * (y1 - y0);
    }

    const std::shared_ptr<std::pmr::vector<CalibrationData>> GetCalibrationTable() const override {
        return calibration_table_;
    }

    const InterpolationMethod GetInterpolationMethod() const override {
        return INTERPOLATION_METHOD;
    }
};

} // namespace eerie_leap::utilities::voltage_interpolator
