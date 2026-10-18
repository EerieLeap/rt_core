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
    *calibration_data = { { 0.501F, 0.469F }, { 1.0F, 0.968F }, { 2.0F, 1.970F }, { 3.002F, 2.98F }, { 4.003F, 4.01F },
        { 5.0F, 5.0F } };

    return make_shared_pmr<AdcCalibrator>(mr, InterpolationMethod::LINEAR, std::move(calibration_data));
}

} // namespace eerie_leap::domain::adc_domain::configuration
