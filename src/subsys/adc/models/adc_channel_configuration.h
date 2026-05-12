#pragma once

#include <memory>

#include "subsys/adc/utilities/adc_calibrator.h"

namespace eerie_leap::subsys::adc::models {

using eerie_leap::subsys::adc::utilities::AdcCalibrator;

struct AdcChannelConfiguration {
    std::shared_ptr<AdcCalibrator> calibrator = nullptr;
};

}  // namespace eerie_leap::subsys::adc::models
