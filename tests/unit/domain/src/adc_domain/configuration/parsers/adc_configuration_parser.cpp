#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#include <zephyr/ztest.h>
#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"
#include "domain/adc_domain/configuration/adc_default_configuration.h"
#include "domain/adc_domain/configuration/parsers/adc_configuration_cbor_parser.h"
#include "domain/adc_domain/configuration/parsers/adc_configuration_validator.h"

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::utilities::voltage_interpolator;
using namespace eerie_leap::domain::adc_domain::utilities;
using namespace eerie_leap::domain::adc_domain::models;
using namespace eerie_leap::domain::adc_domain::configuration;
using namespace eerie_leap::domain::adc_domain::configuration::parsers;

ZTEST_SUITE(adc_configuration_parser, NULL, NULL, NULL, NULL, NULL);

namespace {

constexpr size_t kChannelCount = 8;

std::string ExpectedSamplesReason(uint32_t samples) {
    return "Invalid ADC configuration. Samples must be between 1 and "
        + std::to_string(CONFIG_EERIE_LEAP_ADC_SAMPLES_MAX)
        + ", got "
        + std::to_string(samples)
        + ".";
}

// Runs action and returns the reason of the std::invalid_argument it throws; fails if it does not.
template <typename Action>
std::string ExpectInvalidArgument(Action&& action) {
    try {
        action();
    } catch(const std::invalid_argument& e) {
        return e.what();
    }

    zassert_unreachable("no std::invalid_argument thrown");
    return {};
}

void ExpectReasonContains(const std::string& reason, std::string_view expected) {
    zassert_true(reason.find(expected) != std::string::npos,
        "'%s' does not contain '%.*s'", reason.c_str(), static_cast<int>(expected.size()), expected.data());
}

} // namespace

AdcConfiguration adc_configuration_parser_GetTestConfiguration(size_t channel_count = kChannelCount) {
    std::pmr::vector<CalibrationData> adc_calibration_data_samples_1 {
        {0.0, 0.0},
        {5.0, 5.0}
    };

    auto adc_calibration_data_samples_ptr_1 = std::make_shared<std::pmr::vector<CalibrationData>>(adc_calibration_data_samples_1);
    auto adc_calibrator_1 = std::make_shared<AdcCalibrator>(InterpolationMethod::LINEAR, adc_calibration_data_samples_ptr_1);

    std::pmr::vector<CalibrationData> adc_calibration_data_samples_2 {
        {0.501, 0.469},
        {1.0, 0.968},
        {2.0, 1.970},
        {3.002, 2.98},
        {4.003, 4.01},
        {5.0, 5.0}
    };

    auto adc_calibration_data_samples_ptr_2 = std::make_shared<std::pmr::vector<CalibrationData>>(adc_calibration_data_samples_2);
    auto adc_calibrator_2 = std::make_shared<AdcCalibrator>(InterpolationMethod::CUBIC_SPLINE, adc_calibration_data_samples_ptr_2);

    std::vector<std::shared_ptr<AdcChannelConfiguration>> channel_configurations = {
        std::make_shared<AdcChannelConfiguration>(adc_calibrator_1),
        std::make_shared<AdcChannelConfiguration>(adc_calibrator_2),
        std::make_shared<AdcChannelConfiguration>(adc_calibrator_1),
        std::make_shared<AdcChannelConfiguration>(adc_calibrator_1),
        std::make_shared<AdcChannelConfiguration>(adc_calibrator_1),
        std::make_shared<AdcChannelConfiguration>(adc_calibrator_2),
        std::make_shared<AdcChannelConfiguration>(adc_calibrator_2),
        std::make_shared<AdcChannelConfiguration>(adc_calibrator_2),
    };
    channel_configurations.resize(channel_count, channel_configurations.back());

    AdcConfiguration adc_configuration;
    adc_configuration.samples = 40;
    adc_configuration.channel_configurations =
        std::make_shared<std::vector<std::shared_ptr<AdcChannelConfiguration>>>(channel_configurations);

    return adc_configuration;
}

void adc_configuration_parser_CompareCalibrators(const AdcCalibrator& calibrator, const AdcCalibrator& deserialized_calibrator) {
    zassert_equal(deserialized_calibrator.GetInterpolationMethod(), calibrator.GetInterpolationMethod());

    auto calibration_table = calibrator.GetCalibrationTable();
    auto deserialized_calibration_table = deserialized_calibrator.GetCalibrationTable();
    zassert_equal(calibration_table->size(), deserialized_calibration_table->size());

    for(int j = 0; j < calibration_table->size(); j++) {
        zassert_equal(calibration_table->at(j).voltage, deserialized_calibration_table->at(j).voltage);
        zassert_equal(calibration_table->at(j).value, deserialized_calibration_table->at(j).value);
    }
}

void adc_configuration_parser_CompareAdcConfigurations(AdcConfiguration& adc_configuration, AdcConfiguration& deserialized_adc_configuration) {
    zassert_equal(deserialized_adc_configuration.samples, adc_configuration.samples);
    zassert_equal(deserialized_adc_configuration.channel_configurations->size(), adc_configuration.channel_configurations->size());

    for(int i = 0; i < adc_configuration.channel_configurations->size(); i++) {
        adc_configuration_parser_CompareCalibrators(
            *adc_configuration.channel_configurations->at(i)->calibrator,
            *deserialized_adc_configuration.channel_configurations->at(i)->calibrator);
    }
}

ZTEST(adc_configuration_parser, test_CborSerializeDeserialize) {
    AdcConfigurationCborParser adc_configuration_cbor_parser(kChannelCount);

    auto adc_configuration = adc_configuration_parser_GetTestConfiguration();

    auto serialized_adc_configuration = adc_configuration_cbor_parser.Serialize(adc_configuration);
    auto deserialized_adc_configuration = adc_configuration_cbor_parser.Deserialize(Mrm::GetDefaultPmr(), *serialized_adc_configuration.get());

    adc_configuration_parser_CompareAdcConfigurations(adc_configuration, *deserialized_adc_configuration);
}

ZTEST(adc_configuration_parser, test_Deserialize_orders_an_unordered_calibration_table) {
    AdcConfigurationCborParser adc_configuration_cbor_parser(kChannelCount);

    auto adc_configuration = adc_configuration_parser_GetTestConfiguration();
    auto serialized_adc_configuration = adc_configuration_cbor_parser.Serialize(adc_configuration);

    for(auto& adc_channel_config : serialized_adc_configuration->CborAdcChannelConfig_m)
        std::ranges::reverse(adc_channel_config.calibration_table.float32float);

    auto deserialized_adc_configuration = adc_configuration_cbor_parser.Deserialize(Mrm::GetDefaultPmr(), *serialized_adc_configuration);

    adc_configuration_parser_CompareAdcConfigurations(adc_configuration, *deserialized_adc_configuration);
}

ZTEST(adc_configuration_parser, test_Deserialize_rejects_non_monotonic_values) {
    AdcConfigurationCborParser adc_configuration_cbor_parser(kChannelCount);

    auto adc_configuration = adc_configuration_parser_GetTestConfiguration();

    // A tie on a spline channel: two readings for different voltages.
    {
        auto serialized_adc_configuration = adc_configuration_cbor_parser.Serialize(adc_configuration);
        auto& table = serialized_adc_configuration->CborAdcChannelConfig_m.at(5).calibration_table.float32float;
        table.at(2).float32float = table.at(1).float32float;

        auto reason = ExpectInvalidArgument([&] {
            adc_configuration_cbor_parser.Deserialize(Mrm::GetDefaultPmr(), *serialized_adc_configuration);
        });
        zassert_str_equal(reason.c_str(),
            "Invalid ADC configuration. Channel: 5. Calibration values must be strictly increasing.");
    }

    // A reversal on a linear channel: the readings fall while the voltages rise.
    {
        auto serialized_adc_configuration = adc_configuration_cbor_parser.Serialize(adc_configuration);
        auto& table = serialized_adc_configuration->CborAdcChannelConfig_m.at(2).calibration_table.float32float;
        std::swap(table.front().float32float, table.back().float32float);

        auto reason = ExpectInvalidArgument([&] {
            adc_configuration_cbor_parser.Deserialize(Mrm::GetDefaultPmr(), *serialized_adc_configuration);
        });
        zassert_str_equal(reason.c_str(),
            "Invalid ADC configuration. Channel: 2. Calibration values must be strictly increasing.");
    }

    // The same rule guards Serialize, so a non-monotonic table is never stored.
    {
        auto falling_calibrator = std::make_shared<AdcCalibrator>(InterpolationMethod::LINEAR,
            std::make_shared<std::pmr::vector<CalibrationData>>(std::pmr::vector<CalibrationData> { {0.0F, 5.0F}, {5.0F, 0.0F} }));

        auto falling_configuration = adc_configuration_parser_GetTestConfiguration();
        falling_configuration.channel_configurations->at(3) = std::make_shared<AdcChannelConfiguration>(falling_calibrator);

        auto reason = ExpectInvalidArgument([&] {
            adc_configuration_cbor_parser.Serialize(falling_configuration);
        });
        ExpectReasonContains(reason, "Channel: 3. Calibration values must be strictly increasing.");
    }
}

ZTEST(adc_configuration_parser, test_Deserialize_rejects_duplicate_voltages) {
    AdcConfigurationCborParser adc_configuration_cbor_parser(kChannelCount);

    auto adc_configuration = adc_configuration_parser_GetTestConfiguration();
    auto serialized_adc_configuration = adc_configuration_cbor_parser.Serialize(adc_configuration);
    auto& table = serialized_adc_configuration->CborAdcChannelConfig_m.at(1).calibration_table.float32float;
    table.at(3).float32float_key = table.at(2).float32float_key;

    auto reason = ExpectInvalidArgument([&] {
        adc_configuration_cbor_parser.Deserialize(Mrm::GetDefaultPmr(), *serialized_adc_configuration);
    });
    zassert_str_equal(reason.c_str(),
        "Invalid ADC configuration. Channel: 1. Calibration voltages must be strictly increasing.");
}

ZTEST(adc_configuration_parser, test_Deserialize_rejects_values_that_saturate_together) {
    AdcConfigurationCborParser adc_configuration_cbor_parser(kChannelCount);

    auto adc_configuration = adc_configuration_parser_GetTestConfiguration();
    auto serialized_adc_configuration = adc_configuration_cbor_parser.Serialize(adc_configuration);

    // Both readings lie beyond the sensor maximum, so both map to the top of the ADC range.
    auto& table = serialized_adc_configuration->CborAdcChannelConfig_m.at(4).calibration_table.float32float;
    table.back().float32float = CONFIG_EERIE_LEAP_SENSOR_VOLTAGE_MAX_MV / 1000.0F + 1.0F;
    table.push_back({ .float32float_key = 6.0F, .float32float = CONFIG_EERIE_LEAP_SENSOR_VOLTAGE_MAX_MV / 1000.0F + 2.0F });

    auto reason = ExpectInvalidArgument([&] {
        adc_configuration_cbor_parser.Deserialize(Mrm::GetDefaultPmr(), *serialized_adc_configuration);
    });
    ExpectReasonContains(reason, "Channel: 4. ");
}

ZTEST(adc_configuration_parser, test_Deserialize_loads_a_value_beyond_the_sensor_range) {
    // A table recorded before calibration readings were clamped: its top reading lies above the
    // sensor range. It loads unchanged (with a warning) and its point saturates at the ADC maximum.
    AdcConfigurationCborParser adc_configuration_cbor_parser(kChannelCount);

    auto adc_configuration = adc_configuration_parser_GetTestConfiguration();
    auto serialized_adc_configuration = adc_configuration_cbor_parser.Serialize(adc_configuration);

    auto& table = serialized_adc_configuration->CborAdcChannelConfig_m.at(0).calibration_table.float32float;
    zassert_equal(table.back().float32float_key, 5.0F);
    table.back().float32float = 5.05F;

    auto deserialized_adc_configuration = adc_configuration_cbor_parser.Deserialize(
        Mrm::GetDefaultPmr(), *serialized_adc_configuration);

    const auto& calibrator = *deserialized_adc_configuration->channel_configurations->at(0)->calibrator;
    zassert_equal(calibrator.GetCalibrationTable()->back().value, 5.05F);

    // The reading that recorded 5.05 at a true 5.0 V, and anything above it, maps back to 5.0 V.
    const auto& range = calibrator.GetRange();
    zassert_within(calibrator.InterpolateToCalibratedRange(5.05F * range.adc_max_v / range.sensor_max_v), 5.0F, 1e-3F);
    zassert_within(calibrator.InterpolateToCalibratedRange(range.adc_max_v), 5.0F, 1e-3F);

    // It stores back unchanged.
    auto reserialized_adc_configuration = adc_configuration_cbor_parser.Serialize(*deserialized_adc_configuration);
    zassert_equal(reserialized_adc_configuration->CborAdcChannelConfig_m.at(0).calibration_table.float32float.back().float32float, 5.05F);
}

ZTEST(adc_configuration_parser, test_Deserialize_rejects_non_finite_points) {
    AdcConfigurationCborParser adc_configuration_cbor_parser(kChannelCount);

    auto adc_configuration = adc_configuration_parser_GetTestConfiguration();
    auto serialized_adc_configuration = adc_configuration_cbor_parser.Serialize(adc_configuration);
    serialized_adc_configuration->CborAdcChannelConfig_m.at(6).calibration_table.float32float.at(2).float32float =
        std::numeric_limits<float>::quiet_NaN();

    auto reason = ExpectInvalidArgument([&] {
        adc_configuration_cbor_parser.Deserialize(Mrm::GetDefaultPmr(), *serialized_adc_configuration);
    });
    zassert_str_equal(reason.c_str(),
        "Invalid ADC configuration. Channel: 6. Calibration table must hold finite numbers.");
}

ZTEST(adc_configuration_parser, test_Deserialize_rejects_samples_out_of_range) {
    AdcConfigurationCborParser adc_configuration_cbor_parser(kChannelCount);

    auto adc_configuration = adc_configuration_parser_GetTestConfiguration();
    auto serialized_adc_configuration = adc_configuration_cbor_parser.Serialize(adc_configuration);

    // 65537 is checked as stored: narrowed to uint16_t it would load as 1.
    for(uint32_t samples : { 0U, static_cast<uint32_t>(CONFIG_EERIE_LEAP_ADC_SAMPLES_MAX) + 1U, 65537U }) {
        serialized_adc_configuration->samples = samples;

        auto reason = ExpectInvalidArgument([&] {
            adc_configuration_cbor_parser.Deserialize(Mrm::GetDefaultPmr(), *serialized_adc_configuration);
        });
        zassert_str_equal(reason.c_str(), ExpectedSamplesReason(samples).c_str());
    }

    // Both ends of the range load.
    for(uint32_t samples : { 1U, static_cast<uint32_t>(CONFIG_EERIE_LEAP_ADC_SAMPLES_MAX) }) {
        serialized_adc_configuration->samples = samples;

        auto deserialized_adc_configuration = adc_configuration_cbor_parser.Deserialize(
            Mrm::GetDefaultPmr(), *serialized_adc_configuration);
        zassert_equal(deserialized_adc_configuration->samples, samples);
    }

    // Serialize applies the same bounds.
    for(uint16_t samples : { uint16_t { 0 }, static_cast<uint16_t>(CONFIG_EERIE_LEAP_ADC_SAMPLES_MAX + 1) }) {
        adc_configuration.samples = samples;

        auto reason = ExpectInvalidArgument([&] {
            adc_configuration_cbor_parser.Serialize(adc_configuration);
        });
        zassert_str_equal(reason.c_str(), ExpectedSamplesReason(samples).c_str());
    }
}

ZTEST(adc_configuration_parser, test_Deserialize_pads_missing_channels) {
    constexpr size_t kConfiguredChannelCount = 4;
    constexpr size_t kDtChannelCount = 6;

    AdcConfigurationCborParser adc_configuration_cbor_parser(kDtChannelCount);

    auto adc_configuration = adc_configuration_parser_GetTestConfiguration(kConfiguredChannelCount);
    auto serialized_adc_configuration = adc_configuration_cbor_parser.Serialize(adc_configuration);
    zassert_equal(serialized_adc_configuration->CborAdcChannelConfig_m.size(), kConfiguredChannelCount);

    auto deserialized_adc_configuration = adc_configuration_cbor_parser.Deserialize(
        Mrm::GetDefaultPmr(), *serialized_adc_configuration);

    zassert_equal(deserialized_adc_configuration->samples, adc_configuration.samples);
    zassert_equal(deserialized_adc_configuration->channel_configurations->size(), kDtChannelCount);

    for(size_t i = 0; i < kConfiguredChannelCount; i++) {
        adc_configuration_parser_CompareCalibrators(
            *adc_configuration.channel_configurations->at(i)->calibrator,
            *deserialized_adc_configuration->channel_configurations->at(i)->calibrator);
    }

    auto default_calibrator = AdcDefaultConfiguration::CreateCalibrator(Mrm::GetDefaultPmr());
    for(size_t i = kConfiguredChannelCount; i < kDtChannelCount; i++) {
        auto channel_configuration = deserialized_adc_configuration->channel_configurations->at(i);

        zassert_not_null(channel_configuration.get());
        zassert_not_null(channel_configuration->calibrator.get());
        adc_configuration_parser_CompareCalibrators(*default_calibrator, *channel_configuration->calibrator);
    }

    // The padded configuration is a valid one, so storing it again keeps all six channels.
    auto reserialized_adc_configuration = adc_configuration_cbor_parser.Serialize(*deserialized_adc_configuration);
    zassert_equal(reserialized_adc_configuration->CborAdcChannelConfig_m.size(), kDtChannelCount);
}

ZTEST(adc_configuration_parser, test_Deserialize_rejects_too_many_channels) {
    constexpr size_t kConfiguredChannelCount = 7;
    constexpr size_t kDtChannelCount = 6;
    const std::string expected_reason = "Invalid ADC configuration. 7 channels configured, the device tree has 6.";

    AdcConfigurationCborParser adc_configuration_cbor_parser(kDtChannelCount);
    auto adc_configuration = adc_configuration_parser_GetTestConfiguration(kConfiguredChannelCount);

    auto serialize_reason = ExpectInvalidArgument([&] {
        adc_configuration_cbor_parser.Serialize(adc_configuration);
    });
    zassert_str_equal(serialize_reason.c_str(), expected_reason.c_str());

    // Stored by a unit whose device tree has more channels.
    auto serialized_adc_configuration = AdcConfigurationCborParser(kConfiguredChannelCount).Serialize(adc_configuration);

    auto deserialize_reason = ExpectInvalidArgument([&] {
        adc_configuration_cbor_parser.Deserialize(Mrm::GetDefaultPmr(), *serialized_adc_configuration);
    });
    zassert_str_equal(deserialize_reason.c_str(), expected_reason.c_str());
}

ZTEST(adc_configuration_parser, test_Validate_requires_a_calibrator_per_channel) {
    auto adc_configuration = adc_configuration_parser_GetTestConfiguration();
    adc_configuration.channel_configurations->at(7) = std::make_shared<AdcChannelConfiguration>();

    auto reason = ExpectInvalidArgument([&] {
        AdcConfigurationValidator::Validate(adc_configuration, kChannelCount);
    });
    ExpectReasonContains(reason, "Channel: 7. ");

    adc_configuration.channel_configurations = nullptr;

    reason = ExpectInvalidArgument([&] {
        AdcConfigurationValidator::Validate(adc_configuration, kChannelCount);
    });
    zassert_str_equal(reason.c_str(), "Invalid ADC configuration. Channel configurations are missing.");
}
