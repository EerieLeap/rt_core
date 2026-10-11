#pragma once

#include <cstddef>
#include <cstdint>

#include "domain/adc_domain/models/adc_configuration.h"

namespace eerie_leap::domain::adc_domain::configuration::parsers {

using eerie_leap::domain::adc_domain::models::AdcConfiguration;
using eerie_leap::domain::adc_domain::utilities::AdcCalibrator;

// channel_count is the device tree's; a configuration may cover fewer channels, never more.
class AdcConfigurationValidator {
private:
    static void ValidateCalibrationTable(size_t channel, const AdcCalibrator& calibrator);
    static void ValidateChannels(const AdcConfiguration& configuration, size_t channel_count);

public:
    // Takes the stored width, so a value is checked before it is narrowed into AdcConfiguration.
    static void ValidateSamples(uint32_t samples);
    static void ValidateChannelCount(size_t configured_channel_count, size_t channel_count);
    static void Validate(const AdcConfiguration& configuration, size_t channel_count);
};

} // namespace eerie_leap::domain::adc_domain::configuration::parsers
