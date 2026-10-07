#pragma once

#include <memory>
#include <memory_resource>
#include <vector>
#include <algorithm>

#include "i_voltage_interpolator.h"
#include "calibration_data.h"
#include "interpolation_method.h"

namespace eerie_leap::utilities::voltage_interpolator {

class CubicSplineVoltageInterpolator : public IVoltageInterpolator {
private:
    struct CubicCoefficients{
        float a, b, c, d; // cubic coefficients: a + b*t + c*t^2 + d*t^3
    };

    static const InterpolationMethod INTERPOLATION_METHOD = InterpolationMethod::CUBIC_SPLINE;
    std::shared_ptr<std::pmr::vector<CalibrationData>> calibration_table_;
    // Lives next to the table it was computed from.
    std::pmr::vector<CubicCoefficients> coefficients_;

public:
    explicit CubicSplineVoltageInterpolator(std::shared_ptr<std::pmr::vector<CalibrationData>> calibration_table)
        : calibration_table_(std::move(calibration_table)),
        coefficients_(calibration_table_ != nullptr
            ? calibration_table_->get_allocator().resource()
            : std::pmr::get_default_resource()) {

        if(!calibration_table_ || calibration_table_->size() < 2)
            throw std::invalid_argument("Calibration data is missing or invalid.");

        const auto& table = *calibration_table_;
        auto* const resource = table.get_allocator().resource();

        const size_t n = table.size();
        std::pmr::vector<float> h(n - 1, resource);
        std::pmr::vector<float> alpha(n - 1, resource);
        std::pmr::vector<float> l(n, resource);
        std::pmr::vector<float> mu(n, resource);
        std::pmr::vector<float> z(n, resource);
        coefficients_.resize(n);

        for(size_t i = 0; i < n; ++i)
            coefficients_[i].a = table[i].value;

        for(size_t i = 0; i < n - 1; ++i)
            h[i] = table[i + 1].voltage - table[i].voltage;

        for(size_t i = 1; i < n - 1; ++i)
            alpha[i] = (3.0f / h[i]) * (coefficients_[i + 1].a - coefficients_[i].a) - (3.0f / h[i - 1]) * (coefficients_[i].a - coefficients_[i - 1].a);

        l[0] = 1.0f;
        mu[0] = 0.0f;
        z[0] = 0.0f;

        for(size_t i = 1; i < n - 1; ++i) {
            l[i] = 2.0f * (table[i + 1].voltage - table[i - 1].voltage) - h[i - 1] * mu[i - 1];
            mu[i] = h[i] / l[i];
            z[i] = (alpha[i] - h[i - 1] * z[i - 1]) / l[i];
        }

        l[n - 1] = 1.0f;
        z[n - 1] = 0.0f;
        coefficients_[n - 1].c = 0.0f;

        for(int j = n - 2; j >= 0; --j) {
            coefficients_[j].c = z[j] - mu[j] * coefficients_[j + 1].c;
            coefficients_[j].b = (coefficients_[j + 1].a - coefficients_[j].a) / h[j] - h[j] * (coefficients_[j + 1].c + 2.0f * coefficients_[j].c) / 3.0f;
            coefficients_[j].d = (coefficients_[j + 1].c - coefficients_[j].c) / (3.0f * h[j]);
        }
    }

    float Interpolate(float voltage, bool clamp_to_ends = false) const override {
        const auto& table = *calibration_table_;

        if(clamp_to_ends) {
            if(voltage <= table.front().voltage)
                return table.front().value;
            if(voltage >= table.back().voltage)
                return table.back().value;
        }

        auto it = std::upper_bound(table.begin(), table.end(), voltage,
            [](float val, const CalibrationData& d) { return val < d.voltage; });

        // Keep the segment inside the table; out-of-range voltages extrapolate along the nearest one.
        const size_t last_segment = table.size() - 2;
        const size_t i = it == table.begin()
            ? 0
            : std::min(static_cast<size_t>(std::distance(table.begin(), it)) - 1, last_segment);

        float dx = voltage - table[i].voltage;

        return coefficients_[i].a + coefficients_[i].b * dx + coefficients_[i].c * dx * dx + coefficients_[i].d * dx * dx * dx;
    }

    const std::shared_ptr<std::pmr::vector<CalibrationData>> GetCalibrationTable() const override {
        return calibration_table_;
    }

    const InterpolationMethod GetInterpolationMethod() const override {
        return INTERPOLATION_METHOD;
    }
};

} // namespace eerie_leap::utilities::voltage_interpolator
