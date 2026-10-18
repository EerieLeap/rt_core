#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>
#include <zephyr/ztest.h>

#include "subsys/adc/adc_emulator.h"
#include "subsys/adc/adc_factory.hpp"
#include "subsys/adc/adc_manager.h"
#include "subsys/adc/adc_simulator.h"
#include "subsys/device_tree/dt_adc.h"

#include "emulator_support.h"

using namespace eerie_leap::subsys::device_tree;
using namespace eerie_leap::subsys::adc;
using namespace emulator_support;

namespace {

std::optional<std::vector<AdcDTInfo>> no_dt_adcs;

// The two working emulated ADCs; the third device of the overlay only serves the resolution test.
std::shared_ptr<AdcManager> MakeEmulatorManager() {
    ResetEmulator();

    std::vector<std::shared_ptr<IAdc>> adcs{ std::make_shared<AdcEmulator>(DtAdc::Get().value().at(0)),
        std::make_shared<AdcEmulator>(DtAdc::Get().value().at(1)) };

    return std::make_shared<AdcManager>(std::move(adcs));
}

AdcEmulator& EmulatorAt(const AdcManager& manager, int index) {
    return *dynamic_cast<AdcEmulator*>(manager.GetAdcs()[index].get());
}

float SampleOne(const AdcChannelHandle& handle, uint16_t samples = 10) {
    const uint8_t channels[1] = { handle.local_channel };
    AdcSample results[1];
    AdcSampleRequest request{ .channels = channels, .results = results, .samples = samples };

    handle.adc->Sample(request);
    zassert_equal(results[0].error, AdcError::NONE, "local channel %u", handle.local_channel);

    return results[0].volts;
}

} // namespace

ZTEST_SUITE(adc_manager, NULL, NULL, NULL, NULL, NULL);

ZTEST(adc_manager, test_counts_come_from_the_device_tree) {
    auto manager = MakeEmulatorManager();

    zassert_equal(manager->GetAdcCount(), 2);
    zassert_equal(manager->GetChannelCount(), kWorkingChannelCount);
    zassert_equal(manager->GetAdcs().size(), 2);
    zassert_equal(manager->GetAdcs()[0]->GetChannelCount(), kFirstAdcChannels);
    zassert_equal(manager->GetAdcs()[1]->GetChannelCount(), kSecondAdcChannels);
}

ZTEST(adc_manager, test_Initialize_succeeds) {
    auto manager = MakeEmulatorManager();

    zassert_true(manager->Initialize());

    for(const auto& adc : manager->GetAdcs())
        zassert_true(adc->GetCapabilities().burst, "every device was probed");
}

ZTEST(adc_manager, test_Initialize_fails_when_one_adc_is_rejected) {
    ResetEmulator();

    std::vector<std::shared_ptr<IAdc>> adcs;
    for(const auto& info : DtAdc::Get().value())
        adcs.push_back(std::make_shared<AdcEmulator>(info));

    AdcManager manager(std::move(adcs));

    zassert_equal(manager.GetAdcCount(), kDtAdcCount);
    zassert_equal(manager.GetChannelCount(), kDtChannelCount);
    zassert_false(manager.Initialize(), "the third device has no resolution");
}

ZTEST(adc_manager, test_rejects_a_null_adc) {
    bool threw = false;
    try {
        AdcManager manager({ std::make_shared<AdcSimulator>(), nullptr });
    } catch(const std::invalid_argument&) {
        threw = true;
    }

    zassert_true(threw);
}

ZTEST(adc_manager, test_Resolve_maps_global_to_local) {
    auto manager = MakeEmulatorManager();
    zassert_true(manager->Initialize());

    for(int channel = 0; channel < kWorkingChannelCount; ++channel) {
        auto handle = manager->Resolve(channel);

        zassert_not_null(handle.adc);
        zassert_equal(handle.adc, manager->GetAdcs()[channel / kFirstAdcChannels].get(), "channel %d", channel);
        zassert_equal(handle.local_channel, channel % kFirstAdcChannels, "channel %d", channel);
    }
}

ZTEST(adc_manager, test_Resolve_rejects_out_of_range_channels) {
    auto manager = MakeEmulatorManager();
    zassert_true(manager->Initialize());

    for(int channel : { -1, kWorkingChannelCount, 100 }) {
        bool threw = false;
        try {
            manager->Resolve(channel);
        } catch(const std::invalid_argument&) {
            threw = true;
        }

        zassert_true(threw, "channel %d should be rejected", channel);
    }
}

ZTEST(adc_manager, test_Sample_spans_multiple_adcs) {
    auto manager = MakeEmulatorManager();
    zassert_true(manager->Initialize());

    auto last_of_first = manager->Resolve(kFirstAdcChannels - 1);
    auto first_of_second = manager->Resolve(kFirstAdcChannels);
    auto second_of_second = manager->Resolve(kFirstAdcChannels + 1);

    zassert_equal(last_of_first.local_channel, kFirstAdcChannels - 1);
    zassert_equal(first_of_second.local_channel, 0, "the first channel of the second ADC must map to local index 0");
    zassert_equal(second_of_second.local_channel, 1);
    zassert_true(last_of_first.adc != first_of_second.adc, "channels must resolve to different ADCs");
    zassert_equal(first_of_second.adc, second_of_second.adc);

    // Distinct constants per device prove the stimulus and the conversion meet on the right device.
    DriveConstant(EmulatorAt(*manager, 0), 0, kFirstAdcChannels - 1, 500);
    DriveConstant(EmulatorAt(*manager, 1), 1, 0, 1500);
    DriveConstant(EmulatorAt(*manager, 1), 1, 1, 2500);

    float last_of_first_volts = SampleOne(last_of_first);
    float first_of_second_volts = SampleOne(first_of_second);
    float second_of_second_volts = SampleOne(second_of_second);

    zassert_within(last_of_first_volts, 0.5F, kEmulTolerance);
    zassert_within(first_of_second_volts, 1.5F, kEmulTolerance);
    zassert_within(second_of_second_volts, 2.5F, kEmulTolerance);
}

ZTEST(adc_manager, test_every_channel_reads_within_range) {
    auto manager = MakeEmulatorManager();
    zassert_true(manager->Initialize());

    for(int channel = 0; channel < kWorkingChannelCount; ++channel) {
        auto handle = manager->Resolve(channel);

        for(int i = 0; i < 20; ++i) {
            float volts = SampleOne(handle);
            zassert_between_inclusive(volts, 0.0F, 3.3F, "channel %d out of range", channel);
        }
    }
}

ZTEST(adc_manager, test_every_channel_produces_varying_readings) {
    auto manager = MakeEmulatorManager();
    zassert_true(manager->Initialize());

    // Channels on the second ADC only vary if the emulator stimulus reaches the
    // right device with the right local channel.
    for(int channel = 0; channel < kWorkingChannelCount; ++channel) {
        auto handle = manager->Resolve(channel);

        float first = SampleOne(handle);
        bool changed = false;
        for(int i = 0; i < 20 && !changed; ++i)
            changed = SampleOne(handle) != first;

        zassert_true(changed, "channel %d returned a constant value", channel);
    }
}

ZTEST_SUITE(adc_factory, NULL, NULL, NULL, NULL, NULL);

ZTEST(adc_factory, test_Create_uses_the_device_tree_when_available) {
    ResetEmulator();

    AdcFactory factory(DtAdc::Get);
    auto manager = factory.Create();

    zassert_not_null(manager.get());
    zassert_equal(manager->GetAdcCount(), kDtAdcCount, "one device per device-tree ADC");
    zassert_equal(manager->GetChannelCount(), kDtChannelCount);

    for(const auto& adc : manager->GetAdcs())
        zassert_not_null(dynamic_cast<AdcEmulator*>(adc.get()));
}

ZTEST(adc_factory, test_Create_falls_back_to_the_simulator) {
    AdcFactory factory([]() -> std::optional<std::vector<AdcDTInfo>>& {
        return no_dt_adcs;
    });
    auto manager = factory.Create();

    zassert_not_null(manager.get());
    zassert_equal(manager->GetAdcCount(), 1);
    zassert_not_null(dynamic_cast<AdcSimulator*>(manager->GetAdcs()[0].get()));
    zassert_true(manager->Initialize());
}

ZTEST(adc_factory, test_Create_falls_back_when_no_provider_is_given) {
    AdcFactory factory(nullptr);
    auto manager = factory.Create();

    zassert_equal(manager->GetAdcCount(), 1);
    zassert_not_null(dynamic_cast<AdcSimulator*>(manager->GetAdcs()[0].get()));
}
