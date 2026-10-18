#include <cerrno>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>
#include <zephyr/ztest.h>

#include "subsys/adc/adc_factory.hpp"
#include "subsys/adc/adc_manager.h"
#include "subsys/adc/adc_simulator.h"

using namespace eerie_leap::subsys::adc;
using eerie_leap::subsys::device_tree::AdcDTInfo;

namespace {

constexpr int SIMULATOR_CHANNELS = 8;

float SampleOne(IAdc& adc, uint8_t channel, uint16_t samples = 16) {
    const uint8_t channels[1] = { channel };
    AdcSample results[1];
    const AdcSampleRequest request{ .channels = channels, .results = results, .samples = samples };

    adc.Sample(request);
    zassert_equal(results[0].error, AdcError::NONE, "channel %u", channel);

    return results[0].volts;
}

} // namespace

ZTEST_SUITE(adc_simulator, NULL, NULL, NULL, NULL, NULL);

ZTEST(adc_simulator, test_Initialize_succeeds) {
    AdcSimulator adc;

    zassert_true(adc.Initialize());
    zassert_equal(adc.GetChannelCount(), SIMULATOR_CHANNELS);
}

ZTEST(adc_simulator, test_capabilities_are_reported) {
    AdcSimulator adc;
    adc.Initialize();

    const auto capabilities = adc.GetCapabilities();

    zassert_false(capabilities.burst);
    zassert_false(capabilities.multi_channel);
    zassert_equal(capabilities.us_per_conversion, 0);
}

ZTEST(adc_simulator, test_Sample_marks_out_of_range_channels) {
    AdcSimulator adc;
    adc.Initialize();

    const uint8_t channels[3] = { 7, SIMULATOR_CHANNELS, 100 };
    AdcSample results[3];
    const AdcSampleRequest request{ .channels = channels, .results = results, .samples = 16 };
    adc.Sample(request);

    zassert_equal(results[0].error, AdcError::NONE);
    zassert_between_inclusive(results[0].volts, 0.0F, 3.3F);

    for(const int i : { 1, 2 }) {
        zassert_equal(results[i].error, AdcError::CHANNEL_OUT_OF_RANGE, "channel %u should be rejected", channels[i]);
        zassert_equal(results[i].errno_value, -EINVAL);
    }

    zassert_equal(adc.GetStats().requests, 1);
    zassert_equal(adc.GetStats().errors, 2);
}

ZTEST(adc_simulator, test_Sample_marks_unordered_channels) {
    AdcSimulator adc;
    adc.Initialize();

    // The same contract as the hardware ADC: channels ascending, each at most once.
    const uint8_t channels[3] = { 3, 1, 3 };
    AdcSample results[3];
    const AdcSampleRequest request{ .channels = channels, .results = results, .samples = 16 };
    adc.Sample(request);

    zassert_equal(results[0].error, AdcError::NONE);

    for(const int i : { 1, 2 }) {
        zassert_equal(results[i].error, AdcError::CHANNEL_OUT_OF_RANGE, "entry %d should be rejected", i);
        zassert_equal(results[i].errno_value, -EINVAL);
    }

    zassert_equal(adc.GetStats().errors, 2);
    zassert_equal(adc.GetStats().conversions, 1);
}

ZTEST(adc_simulator, test_Sample_accepts_an_empty_request) {
    AdcSimulator adc;
    adc.Initialize();

    adc.Sample(AdcSampleRequest{});

    zassert_equal(adc.GetStats().requests, 1);
    zassert_equal(adc.GetStats().errors, 0);
}

ZTEST(adc_simulator, test_Sample_clamps_samples_to_at_least_one) {
    AdcSimulator adc;
    adc.Initialize();

    const float volts = SampleOne(adc, 0, 0);

    zassert_between_inclusive(volts, 0.0F, 3.3F, "zero samples must still produce a reading");
    zassert_equal(adc.GetStats().conversions, 1);
}

ZTEST(adc_simulator, test_Sample_stays_within_the_adc_range) {
    AdcSimulator adc;
    adc.Initialize();

    for(int channel = 0; channel < adc.GetChannelCount(); ++channel) {
        for(int i = 0; i < 50; ++i) {
            const float volts = SampleOne(adc, channel);
            zassert_between_inclusive(volts, 0.0F, 3.3F);
        }
    }
}

ZTEST(adc_simulator, test_Sample_varies) {
    AdcSimulator adc;
    adc.Initialize();

    const float first = SampleOne(adc, 0);
    bool changed = false;
    for(int i = 0; i < 20 && !changed; ++i)
        changed = SampleOne(adc, 0) != first;

    zassert_true(changed, "simulated readings should not be constant");
}

ZTEST_SUITE(adc_simulator_manager, NULL, NULL, NULL, NULL, NULL);

ZTEST(adc_simulator_manager, test_counts) {
    AdcManager manager({ std::make_shared<AdcSimulator>() });

    zassert_true(manager.Initialize());
    zassert_equal(manager.GetAdcCount(), 1);
    zassert_equal(manager.GetChannelCount(), SIMULATOR_CHANNELS);
    zassert_equal(manager.GetAdcs().size(), 1);
}

ZTEST(adc_simulator_manager, test_rejects_a_null_adc) {
    bool threw = false;
    try {
        const AdcManager manager({ nullptr });
    } catch(const std::invalid_argument&) {
        threw = true;
    }

    zassert_true(threw);
}

ZTEST(adc_simulator_manager, test_Resolve_maps_global_to_local) {
    const auto simulator = std::make_shared<AdcSimulator>();
    AdcManager manager({ simulator });
    manager.Initialize();

    for(int channel = 0; channel < SIMULATOR_CHANNELS; ++channel) {
        const auto handle = manager.Resolve(channel);

        zassert_equal(handle.adc, simulator.get());
        zassert_equal(static_cast<int>(handle.local_channel), channel);
    }
}

ZTEST(adc_simulator_manager, test_Resolve_rejects_out_of_range_channels) {
    AdcManager manager({ std::make_shared<AdcSimulator>() });
    manager.Initialize();

    for(const int channel : { -1, SIMULATOR_CHANNELS, 100 }) {
        bool threw = false;
        try {
            manager.Resolve(channel);
        } catch(const std::invalid_argument&) {
            threw = true;
        }

        zassert_true(threw, "channel %d should be rejected", channel);
    }
}

ZTEST(adc_simulator_manager, test_Resolve_spans_two_simulators) {
    AdcManager manager({ std::make_shared<AdcSimulator>(), std::make_shared<AdcSimulator>() });
    manager.Initialize();

    zassert_equal(manager.GetChannelCount(), 2 * SIMULATOR_CHANNELS);

    const auto last_of_first = manager.Resolve(SIMULATOR_CHANNELS - 1);
    const auto first_of_second = manager.Resolve(SIMULATOR_CHANNELS);

    zassert_equal(last_of_first.adc, manager.GetAdcs()[0].get());
    zassert_equal(last_of_first.local_channel, SIMULATOR_CHANNELS - 1);
    zassert_equal(first_of_second.adc, manager.GetAdcs()[1].get());
    zassert_equal(first_of_second.local_channel, 0);
}

ZTEST(adc_simulator_manager, test_resolved_channels_read_within_range) {
    AdcManager manager({ std::make_shared<AdcSimulator>() });
    manager.Initialize();

    for(int channel = 0; channel < SIMULATOR_CHANNELS; ++channel) {
        const auto handle = manager.Resolve(channel);

        for(int i = 0; i < 25; ++i) {
            const float volts = SampleOne(*handle.adc, handle.local_channel);
            zassert_between_inclusive(volts, 0.0F, 3.3F);
        }
    }
}

#ifndef CONFIG_ADC
// A unit without an ADC driver (rt_gauge) may still describe ADCs in its device tree.
ZTEST(adc_simulator_manager, test_factory_ignores_device_tree_without_a_driver) {
    static std::optional<std::vector<AdcDTInfo>> dt_adcs = std::vector<AdcDTInfo>{ AdcDTInfo{ .adc_device = nullptr,
        .channel_configs = { adc_channel_cfg{} },
        .references_mv = { 3300 },
        .resolutions = { 12 },
        .oversampling = { 0 } } };

    AdcFactory factory([] -> std::optional<std::vector<AdcDTInfo>>& {
        return dt_adcs;
    });
    const auto manager = factory.Create();

    zassert_equal(manager->GetAdcCount(), 1);
    zassert_not_null(dynamic_cast<AdcSimulator*>(manager->GetAdcs()[0].get()));
    zassert_true(manager->Initialize());
    zassert_equal(manager->GetChannelCount(), SIMULATOR_CHANNELS);
}
#endif
