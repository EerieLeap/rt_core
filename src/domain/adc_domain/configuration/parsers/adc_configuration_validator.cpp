#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>

#include "adc_configuration_validator.h"

namespace eerie_leap::domain::adc_domain::configuration::parsers {

static constexpr uint32_t SAMPLES_MAX = CONFIG_EERIE_LEAP_ADC_SAMPLES_MAX;

[[noreturn]] static void InvalidConfiguration(size_t channel, std::string_view message) {
    throw std::invalid_argument(
        "Invalid ADC configuration. Channel: " + std::to_string(channel) + ". " + std::string(message)
    );
}

void AdcConfigurationValidator::ValidateSamples(uint32_t samples) {
    if(samples < 1 || samples > SAMPLES_MAX)
        throw std::invalid_argument(
            "Invalid ADC configuration. Samples must be between 1 and " + std::to_string(SAMPLES_MAX) + ", got "
            + std::to_string(samples) + "."
        );
}

void AdcConfigurationValidator::ValidateChannelCount(size_t configured_channel_count, size_t channel_count) {
    if(configured_channel_count > channel_count)
        throw std::invalid_argument(
            "Invalid ADC configuration. " + std::to_string(configured_channel_count)
            + " channels configured, the device tree has " + std::to_string(channel_count) + "."
        );
}

// Both interpolators binary search, and the spline divides by the distance between neighbours, so
// both columns must be strictly increasing (the parser orders a stored table by its voltages).
void AdcConfigurationValidator::ValidateCalibrationTable(size_t channel, const AdcCalibrator& calibrator) {
    const auto& calibration_table = *calibrator.GetCalibrationTable();
    const auto& range = calibrator.GetRange();

    if(calibration_table.size() < 2)
        InvalidConfiguration(channel, "Calibration table must have at least 2 points.");

    for(size_t i = 0; i < calibration_table.size(); i++) {
        const auto& point = calibration_table[i];

        if(!std::isfinite(point.voltage) || !std::isfinite(point.value))
            InvalidConfiguration(channel, "Calibration table must hold finite numbers.");

        if(i == 0)
            continue;

        const auto& previous = calibration_table[i - 1];

        if(!(previous.voltage < point.voltage))
            InvalidConfiguration(channel, "Calibration voltages must be strictly increasing.");

        if(!(previous.value < point.value))
            InvalidConfiguration(channel, "Calibration values must be strictly increasing.");

        // Values beyond the sensor range saturate at the end of the ADC range.
        if(!(range.ToAdcVolts(previous.value) < range.ToAdcVolts(point.value)))
            InvalidConfiguration(channel, "Calibration values beyond the sensor range map to the same ADC voltage.");
    }
}

void AdcConfigurationValidator::ValidateChannels(const AdcConfiguration& configuration, size_t channel_count) {
    if(configuration.channel_configurations == nullptr)
        throw std::invalid_argument("Invalid ADC configuration. Channel configurations are missing.");

    ValidateChannelCount(configuration.channel_configurations->size(), channel_count);

    for(size_t i = 0; i < configuration.channel_configurations->size(); i++) {
        const auto& channel_configuration = configuration.channel_configurations->at(i);

        if(channel_configuration == nullptr || channel_configuration->calibrator == nullptr)
            InvalidConfiguration(i, "Invalid interpolation method.");

        ValidateCalibrationTable(i, *channel_configuration->calibrator);
    }
}

void AdcConfigurationValidator::Validate(const AdcConfiguration& configuration, size_t channel_count) {
    ValidateSamples(configuration.samples);
    ValidateChannels(configuration, channel_count);
}

} // namespace eerie_leap::domain::adc_domain::configuration::parsers
