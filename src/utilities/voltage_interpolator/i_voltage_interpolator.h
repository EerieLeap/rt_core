#pragma once

#include <memory>
#include <memory_resource>
#include <vector>

#include "calibration_data.h"
#include "interpolation_method.h"

namespace eerie_leap::utilities::voltage_interpolator {

class IVoltageInterpolator {
public:
    virtual ~IVoltageInterpolator() = default;

    virtual float Interpolate(float voltage, bool clamp_to_ends = false) const = 0;
    virtual std::shared_ptr<std::pmr::vector<CalibrationData>> GetCalibrationTable() const = 0;
    virtual InterpolationMethod GetInterpolationMethod() const = 0;
};

} // namespace eerie_leap::utilities::voltage_interpolator
