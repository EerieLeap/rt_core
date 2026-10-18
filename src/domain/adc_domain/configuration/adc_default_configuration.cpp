#include <eerie_memory.hpp>

#include "utilities/voltage_interpolator/calibration_data.h"
#include "utilities/voltage_interpolator/interpolation_method.h"

#include "adc_default_configuration.h"

namespace eerie_leap::domain::adc_domain::configuration {

using namespace eerie_memory;
using namespace eerie_leap::utilities::voltage_interpolator;

// TODO: Refine default configuration
std::shared_ptr<AdcCalibrator> AdcDefaultConfiguration::CreateCalibrator(std::pmr::memory_resource* mr) {
    const auto calibration_data = make_shared_pmr<std::pmr::vector<CalibrationData>>(mr);
    *calibration_data = { { 0.501f, 0.469f }, { 1.0f, 0.968f }, { 2.0f, 1.970f }, { 3.002f, 2.98f }, { 4.003f, 4.01f },
        { 5.0f, 5.0f } };

    return make_shared_pmr<AdcCalibrator>(mr, InterpolationMethod::LINEAR, std::move(calibration_data));
}

} // namespace eerie_leap::domain::adc_domain::configuration
