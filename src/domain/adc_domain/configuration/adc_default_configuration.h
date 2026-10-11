#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <memory_resource>

#include "domain/adc_domain/utilities/adc_calibrator.h"

namespace eerie_leap::domain::adc_domain::configuration {

using eerie_leap::domain::adc_domain::utilities::AdcCalibrator;

// What a channel nobody has calibrated gets: every channel of a new unit, and the channels a
// stored configuration has fewer of than the device tree.
class AdcDefaultConfiguration {
public:
    static constexpr uint16_t SAMPLES = std::min(40, CONFIG_EERIE_LEAP_ADC_SAMPLES_MAX);

    static std::shared_ptr<AdcCalibrator> CreateCalibrator(std::pmr::memory_resource* mr);
};

} // namespace eerie_leap::domain::adc_domain::configuration
