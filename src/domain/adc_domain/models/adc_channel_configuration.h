#pragma once

#include <memory>

#include "domain/adc_domain/utilities/adc_calibrator.h"

namespace eerie_leap::domain::adc_domain::models {

using eerie_leap::domain::adc_domain::utilities::AdcCalibrator;

struct AdcChannelConfiguration {
    std::shared_ptr<AdcCalibrator> calibrator = nullptr;
};

}  // namespace eerie_leap::domain::adc_domain::models
