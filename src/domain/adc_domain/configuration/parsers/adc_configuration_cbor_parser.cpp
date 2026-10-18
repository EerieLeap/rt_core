#include <algorithm>
#include <stdexcept>
#include <string>

#include <zephyr/logging/log.h>

#include "domain/adc_domain/configuration/adc_default_configuration.h"
#include "utilities/memory/memory_resource_manager.h"

#include "adc_configuration_cbor_parser.h"
#include "adc_configuration_validator.h"

namespace eerie_leap::domain::adc_domain::configuration::parsers {

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::utilities::voltage_interpolator;
using namespace eerie_leap::domain::adc_domain::models;

LOG_MODULE_REGISTER(adc_configuration_parser_logger);

AdcConfigurationCborParser::AdcConfigurationCborParser(size_t channel_count) :
    channel_count_(channel_count) {}

// Tables recorded before calibration readings were clamped can hold a value beyond the sensor
// range. It loads, but saturates at the end of the ADC range, which moves the end of its segment.
static void WarnSaturatedValues(size_t channel, const AdcCalibrator& calibrator) {
    const auto& range = calibrator.GetRange();

    for(const auto& point : *calibrator.GetCalibrationTable()) {
        if(point.value < 0.0F || point.value > range.sensor_max_v) {
            LOG_WRN(
                "ADC channel %zu calibration has a value beyond the sensor range; it saturates at the end of the ADC "
                "range. Recalibrate the channel.",
                channel
            );
            return;
        }
    }
}

pmr_unique_ptr<CborAdcConfig> AdcConfigurationCborParser::Serialize(const AdcConfiguration& configuration) {
    // Also guarantees every channel a calibrator with a strictly increasing table.
    AdcConfigurationValidator::Validate(configuration, channel_count_);
    auto config = make_unique_pmr<CborAdcConfig>(Mrm::GetExtPmr());

    config->samples = configuration.samples;

    for(const auto& channel_configuration : *configuration.channel_configurations) {
        CborAdcChannelConfig adc_channel_config(std::allocator_arg, Mrm::GetExtPmr());

        const auto& calibrator = *channel_configuration->calibrator;

        adc_channel_config.interpolation_method = static_cast<uint32_t>(calibrator.GetInterpolationMethod());
        adc_channel_config.calibration_table_present = true;

        for(const auto& calibration_data : *calibrator.GetCalibrationTable()) {
            adc_channel_config.calibration_table.float32float.push_back(
                { .float32float_key = calibration_data.voltage, .float32float = calibration_data.value }
            );
        }

        config->CborAdcChannelConfig_m.push_back(std::move(adc_channel_config));
    }

    return config;
}

pmr_unique_ptr<AdcConfiguration>
AdcConfigurationCborParser::Deserialize(std::pmr::memory_resource* mr, const CborAdcConfig& config) {
    // Before narrowing: 65537 stored would otherwise load as 1.
    AdcConfigurationValidator::ValidateSamples(config.samples);
    AdcConfigurationValidator::ValidateChannelCount(config.CborAdcChannelConfig_m.size(), channel_count_);

    auto configuration = make_unique_pmr<AdcConfiguration>(mr);

    configuration->samples = static_cast<uint16_t>(config.samples);
    configuration->channel_configurations = make_shared_pmr<std::vector<std::shared_ptr<AdcChannelConfiguration>>>(mr);
    configuration->channel_configurations->reserve(channel_count_);

    for(const auto& adc_channel_config : config.CborAdcChannelConfig_m) {
        auto adc_channel_configuration = make_shared_pmr<AdcChannelConfiguration>(mr);

        auto interpolation_method = static_cast<InterpolationMethod>(adc_channel_config.interpolation_method);
        if(interpolation_method != InterpolationMethod::NONE && adc_channel_config.calibration_table_present) {
            std::pmr::vector<CalibrationData> calibration_table(mr);
            for(const auto& calibration_data : adc_channel_config.calibration_table.float32float) {
                calibration_table.push_back(
                    { .voltage = calibration_data.float32float_key, .value = calibration_data.float32float }
                );
            }

            // An imported table may be stored unordered; the validator requires it ordered.
            std::ranges::sort(calibration_table, {}, &CalibrationData::voltage);

            auto calibration_table_ptr = make_shared_pmr<std::pmr::vector<CalibrationData>>(
                mr, std::move(calibration_table)
            );
            adc_channel_configuration->calibrator = make_shared_pmr<AdcCalibrator>(
                mr, interpolation_method, calibration_table_ptr
            );
            WarnSaturatedValues(configuration->channel_configurations->size(), *adc_channel_configuration->calibrator);
        } else {
            throw std::runtime_error(
                "ADC channel configuration is invalid. Channel: "
                + std::to_string(configuration->channel_configurations->size()) + ". Calibration table is missing."
            );
        }

        configuration->channel_configurations->push_back(std::move(adc_channel_configuration));
    }

    // A firmware update that adds channels keeps the user's calibration of the existing ones.
    const size_t configured_channel_count = configuration->channel_configurations->size();
    if(configured_channel_count < channel_count_) {
        LOG_WRN(
            "ADC configuration has %zu channels, the device tree %zu; the rest get the default calibration.",
            configured_channel_count, channel_count_
        );

        const auto default_channel_configuration = make_shared_pmr<AdcChannelConfiguration>(
            mr, AdcDefaultConfiguration::CreateCalibrator(mr)
        );
        configuration->channel_configurations->resize(channel_count_, default_channel_configuration);
    }

    AdcConfigurationValidator::Validate(*configuration, channel_count_);

    return configuration;
}

} // namespace eerie_leap::domain::adc_domain::configuration::parsers
