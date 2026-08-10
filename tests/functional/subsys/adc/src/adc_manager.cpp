#include <memory>
#include <stdexcept>
#include <vector>
#include <zephyr/ztest.h>

#include "utilities/voltage_interpolator/calibration_data.h"
#include "utilities/voltage_interpolator/interpolation_method.h"

#include "subsys/device_tree/dt_adc.h"
#include "subsys/adc/adc_emulator.h"
#include "subsys/adc/adc_factory.hpp"
#include "subsys/adc/adc_manager.h"
#include "subsys/adc/adc_simulator.h"
#include "subsys/adc/models/adc_configuration.h"

using namespace eerie_leap::utilities::voltage_interpolator;
using namespace eerie_leap::subsys::device_tree;
using namespace eerie_leap::subsys::adc;
using namespace eerie_leap::subsys::adc::models;

namespace {

// The native_sim overlay declares two emulated ADCs: four channels then two.
constexpr int kFirstAdcChannels = 4;
constexpr int kDtChannelCount = 6;

std::optional<std::vector<AdcDTInfo>> no_dt_adcs;

std::shared_ptr<AdcConfiguration> MakeConfiguration(int channels, uint16_t samples = 40) {
    auto calibration_table = std::make_shared<std::pmr::vector<CalibrationData>>(
        std::pmr::vector<CalibrationData> {{0.0F, 0.0F}, {3.3F, 3.3F}});
    auto calibrator = std::make_shared<AdcCalibrator>(InterpolationMethod::LINEAR, calibration_table);

    auto channel_configurations = std::make_shared<std::vector<std::shared_ptr<AdcChannelConfiguration>>>();
    for(int i = 0; i < channels; ++i)
        channel_configurations->push_back(std::make_shared<AdcChannelConfiguration>(calibrator));

    auto configuration = std::make_shared<AdcConfiguration>();
    configuration->samples = samples;
    configuration->channel_configurations = channel_configurations;

    return configuration;
}

std::shared_ptr<AdcEmulatorManager> MakeEmulatorManager() {
    DtAdc::Initialize();

    return std::make_shared<AdcEmulatorManager>(DtAdc::Get().value());
}

} // namespace

ZTEST_SUITE(adc_manager, NULL, NULL, NULL, NULL, NULL);

ZTEST(adc_manager, test_counts_come_from_the_device_tree) {
    auto manager = MakeEmulatorManager();

    zassert_equal(manager->GetAdcCount(), 2);
    zassert_equal(manager->GetChannelCount(), kDtChannelCount);
}

ZTEST(adc_manager, test_Initialize_succeeds) {
    auto manager = MakeEmulatorManager();

    zassert_true(manager->Initialize());
}

ZTEST(adc_manager, test_UpdateConfiguration_rejects_null) {
    auto manager = MakeEmulatorManager();

    bool threw = false;
    try {
        manager->UpdateConfiguration(nullptr);
    } catch(const std::invalid_argument&) {
        threw = true;
    }

    zassert_true(threw);
}

ZTEST(adc_manager, test_GetChannelConfiguration_requires_configuration) {
    auto manager = MakeEmulatorManager();
    manager->Initialize();

    bool threw = false;
    try {
        manager->GetChannelConfiguration(0);
    } catch(const std::invalid_argument&) {
        threw = true;
    }

    zassert_true(threw);
}

ZTEST(adc_manager, test_GetChannelConfiguration_returns_configured_channel) {
    auto configuration = MakeConfiguration(kDtChannelCount);

    auto manager = MakeEmulatorManager();
    manager->UpdateConfiguration(configuration);
    manager->Initialize();

    for(int channel = 0; channel < kDtChannelCount; ++channel)
        zassert_equal(manager->GetChannelConfiguration(channel).get(),
            configuration->channel_configurations->at(channel).get());
}

ZTEST(adc_manager, test_GetAdcForChannel_maps_to_a_local_index) {
    auto manager = MakeEmulatorManager();
    manager->UpdateConfiguration(MakeConfiguration(kDtChannelCount));
    manager->Initialize();

    for(int channel = 0; channel < kDtChannelCount; ++channel) {
        auto [adc, channel_index] = manager->GetAdcForChannel(channel);

        zassert_not_null(adc);
        zassert_equal(channel_index, channel % kFirstAdcChannels);
    }
}

ZTEST(adc_manager, test_GetAdcForChannel_rejects_out_of_range_channels) {
    auto manager = MakeEmulatorManager();
    manager->UpdateConfiguration(MakeConfiguration(kDtChannelCount));
    manager->Initialize();

    for(int channel : { -1, kDtChannelCount, 100 }) {
        bool threw = false;
        try {
            manager->GetAdcForChannel(channel);
        } catch(const std::invalid_argument&) {
            threw = true;
        }

        zassert_true(threw, "channel %d should be rejected", channel);
    }
}

ZTEST(adc_manager, test_channel_validity_is_bounded_by_the_configuration) {
    auto manager = MakeEmulatorManager();
    manager->UpdateConfiguration(MakeConfiguration(2));
    manager->Initialize();

    // Two configured channels even though the device tree exposes four.
    zassert_not_null(manager->GetChannelConfiguration(1).get());

    for(int channel : { 2, 3 }) {
        bool threw = false;
        try {
            manager->GetChannelConfiguration(channel);
        } catch(const std::invalid_argument&) {
            threw = true;
        }

        zassert_true(threw, "unconfigured channel %d should be rejected", channel);
    }
}

ZTEST(adc_manager, test_ResetSamplesCount_requires_configuration) {
    auto manager = MakeEmulatorManager();
    manager->Initialize();

    bool threw = false;
    try {
        manager->ResetSamplesCount();
    } catch(const std::runtime_error&) {
        threw = true;
    }

    zassert_true(threw);
}

ZTEST(adc_manager, test_zero_samples_disables_reading) {
    auto manager = MakeEmulatorManager();
    manager->UpdateConfiguration(MakeConfiguration(kDtChannelCount, 0));
    manager->Initialize();

    auto reader = manager->GetChannelReader(0);

    bool threw = false;
    try {
        reader();
    } catch(const std::runtime_error&) {
        threw = true;
    }

    zassert_true(threw, "a zero sample count must not divide by zero");
}

ZTEST(adc_manager, test_sample_count_can_be_overridden_and_restored) {
    auto manager = MakeEmulatorManager();
    manager->UpdateConfiguration(MakeConfiguration(kDtChannelCount, 8));
    manager->Initialize();

    auto reader = manager->GetChannelReader(0);

    manager->UpdateSamplesCount(1);
    zassert_between_inclusive(reader(), 0.0, 3.3);

    manager->ResetSamplesCount();
    zassert_between_inclusive(reader(), 0.0, 3.3);
}

ZTEST(adc_manager, test_every_channel_reads_within_range) {
    auto manager = MakeEmulatorManager();
    manager->UpdateConfiguration(MakeConfiguration(kDtChannelCount));
    manager->Initialize();

    for(int channel = 0; channel < kDtChannelCount; ++channel) {
        auto reader = manager->GetChannelReader(channel);

        for(int i = 0; i < 20; ++i)
            zassert_between_inclusive(reader(), 0.0, 3.3, "channel %d out of range", channel);
    }
}

ZTEST(adc_manager, test_GetAdcForChannel_spans_multiple_adcs) {
    auto manager = MakeEmulatorManager();
    manager->UpdateConfiguration(MakeConfiguration(kDtChannelCount));
    manager->Initialize();

    auto [first_adc, first_index] = manager->GetAdcForChannel(kFirstAdcChannels - 1);
    auto [second_adc, second_index] = manager->GetAdcForChannel(kFirstAdcChannels);
    auto [third_adc, third_index] = manager->GetAdcForChannel(kFirstAdcChannels + 1);

    zassert_equal(first_index, kFirstAdcChannels - 1);
    zassert_equal(second_index, 0, "the first channel of the second ADC must map to local index 0");
    zassert_equal(third_index, 1);
    zassert_true(first_adc != second_adc, "channels must resolve to different ADCs");
    zassert_equal(second_adc, third_adc);
}

ZTEST(adc_manager, test_every_channel_produces_varying_readings) {
    auto manager = MakeEmulatorManager();
    manager->UpdateConfiguration(MakeConfiguration(kDtChannelCount));
    manager->Initialize();

    // Channels on the second ADC only vary if the emulator stimulus reaches the
    // right device with the right local channel.
    for(int channel = 0; channel < kDtChannelCount; ++channel) {
        auto reader = manager->GetChannelReader(channel);

        float first = reader();
        bool changed = false;
        for(int i = 0; i < 20 && !changed; ++i)
            changed = reader() != first;

        zassert_true(changed, "channel %d returned a constant value", channel);
    }
}

ZTEST_SUITE(adc_factory, NULL, NULL, NULL, NULL, NULL);

ZTEST(adc_factory, test_Create_uses_the_device_tree_when_available) {
    DtAdc::Initialize();

    AdcFactory factory(DtAdc::Get);
    auto manager = factory.Create();

    zassert_not_null(manager.get());
    zassert_not_null(dynamic_cast<AdcEmulatorManager*>(manager.get()));
    zassert_equal(manager->GetChannelCount(), kDtChannelCount);
}

ZTEST(adc_factory, test_Create_falls_back_to_the_simulator) {
    AdcFactory factory([]() -> std::optional<std::vector<AdcDTInfo>>& { return no_dt_adcs; });
    auto manager = factory.Create();

    zassert_not_null(manager.get());
    zassert_not_null(dynamic_cast<AdcSimulatorManager*>(manager.get()));
    zassert_equal(manager->GetAdcCount(), 1);
}

ZTEST(adc_factory, test_Create_falls_back_when_no_provider_is_given) {
    AdcFactory factory(nullptr);
    auto manager = factory.Create();

    zassert_not_null(dynamic_cast<AdcSimulatorManager*>(manager.get()));
}
