// A rate group's ADC channels: one slot per channel that stays put, one Sample() per ADC per tick.
#include <stdexcept>

#include <zephyr/ztest.h>

#include "domain/sensor_domain/services/adc_sample_set.h"
#include "subsys/adc/adc_types.h"

#include "processing_test_support.h"

using namespace eerie_leap::subsys::adc;
using namespace eerie_leap::domain::sensor_domain::services;

using processing_test_support::FakeAdc;

ZTEST_SUITE(adc_sample_set, NULL, NULL, NULL, NULL, NULL);

ZTEST(adc_sample_set, test_Reserve_twice_for_one_channel_returns_the_same_slot) {
    FakeAdc adc(4);
    AdcSampleSet adc_samples;

    const AdcSample* first = adc_samples.Reserve(2, { .adc = &adc, .local_channel = 2 }, 10);
    const AdcSample* second = adc_samples.Reserve(2, { .adc = &adc, .local_channel = 2 }, 10);

    zassert_not_null(first);
    zassert_equal(first, second, "Two sensors on one channel share one conversion");
    zassert_equal(adc_samples.GetAdcCount(), 1);
    zassert_equal(adc_samples.GetChannelCount(), 1);
}

ZTEST(adc_sample_set, test_Reserve_slots_stay_valid_as_channels_are_added) {
    FakeAdc adc_a(4);
    FakeAdc adc_b(4);
    adc_a.volts_base = 0.25F;
    adc_a.volts_step = 0.5F;
    adc_b.volts_base = 2.0F;
    adc_b.volts_step = 0.125F;

    AdcSampleSet adc_samples;

    // Descending, so every reservation lands before the ones already made.
    const AdcSample* a3 = adc_samples.Reserve(3, { .adc = &adc_a, .local_channel = 3 }, 4);
    const AdcSample* a1 = adc_samples.Reserve(1, { .adc = &adc_a, .local_channel = 1 }, 4);
    const AdcSample* b2 = adc_samples.Reserve(6, { .adc = &adc_b, .local_channel = 2 }, 4);
    const AdcSample* a0 = adc_samples.Reserve(0, { .adc = &adc_a, .local_channel = 0 }, 4);
    const AdcSample* b0 = adc_samples.Reserve(4, { .adc = &adc_b, .local_channel = 0 }, 4);

    zassert_equal(adc_samples.Reserve(3, { .adc = &adc_a, .local_channel = 3 }, 4), a3, "Still channel 3's slot");
    zassert_equal(adc_samples.Reserve(6, { .adc = &adc_b, .local_channel = 2 }, 4), b2);

    adc_samples.Seal();
    adc_samples.Acquire();

    // Every slot holds its own channel's result, wherever its channel was inserted.
    zassert_within(a0->volts, 0.25F, 1e-6F);
    zassert_within(a1->volts, 0.75F, 1e-6F);
    zassert_within(a3->volts, 1.75F, 1e-6F);
    zassert_within(b0->volts, 2.0F, 1e-6F);
    zassert_within(b2->volts, 2.25F, 1e-6F);
}

ZTEST(adc_sample_set, test_Seal_packs_the_channels_ascending) {
    FakeAdc adc(4);
    AdcSampleSet adc_samples;

    (void)adc_samples.Reserve(3, { .adc = &adc, .local_channel = 3 }, 8);
    (void)adc_samples.Reserve(0, { .adc = &adc, .local_channel = 0 }, 16);
    (void)adc_samples.Reserve(2, { .adc = &adc, .local_channel = 2 }, 4);
    (void)adc_samples.Reserve(0, { .adc = &adc, .local_channel = 0 }, 1);

    adc_samples.Seal();
    adc_samples.Acquire();

    zassert_equal(adc.requests, 1);
    zassert_equal(adc.last_channel_count, 3, "Channel 0 is requested once");
    zassert_equal(adc.last_channels[0], 0);
    zassert_equal(adc.last_channels[1], 2);
    zassert_equal(adc.last_channels[2], 3);
    zassert_equal(adc.last_samples, 16, "The largest count any reservation asked for");
}

ZTEST(adc_sample_set, test_Acquire_samples_each_adc_once_and_fills_every_slot) {
    FakeAdc adc_a(4);
    FakeAdc adc_b(2);
    adc_a.volts_base = 1.0F;
    adc_a.volts_step = 0.5F;
    adc_a.failing_channel = 1;
    adc_b.volts_base = 3.0F;

    AdcSampleSet adc_samples;
    const AdcSample* a0 = adc_samples.Reserve(0, { .adc = &adc_a, .local_channel = 0 }, 2);
    const AdcSample* a1 = adc_samples.Reserve(1, { .adc = &adc_a, .local_channel = 1 }, 2);
    const AdcSample* a2 = adc_samples.Reserve(2, { .adc = &adc_a, .local_channel = 2 }, 2);
    const AdcSample* b1 = adc_samples.Reserve(5, { .adc = &adc_b, .local_channel = 1 }, 2);

    adc_samples.Acquire();
    zassert_equal(adc_a.requests + adc_b.requests, 0, "Nothing is sampled before Seal()");

    adc_samples.Seal();
    zassert_equal(adc_samples.GetAdcCount(), 2);
    zassert_equal(adc_samples.GetChannelCount(), 4);

    for(int tick = 1; tick <= 3; tick++) {
        adc_samples.Acquire();

        zassert_equal(adc_a.requests, tick, "One Sample() per ADC per Acquire()");
        zassert_equal(adc_b.requests, tick);
    }

    zassert_equal(adc_a.last_channel_count, 3);
    zassert_equal(adc_b.last_channel_count, 1);

    zassert_equal(a0->error, AdcError::NONE);
    zassert_within(a0->volts, 1.0F, 1e-6F);
    zassert_equal(a1->error, AdcError::DRIVER, "Each slot carries its own status");
    zassert_equal(a1->errno_value, -5);
    zassert_equal(a2->error, AdcError::NONE);
    zassert_within(a2->volts, 2.0F, 1e-6F);
    zassert_equal(b1->error, AdcError::NONE);
    zassert_within(b1->volts, 3.0F, 1e-6F);

    // A recovered channel clears its error on the next tick.
    adc_a.failing_channel = -1;
    adc_samples.Acquire();
    zassert_equal(a1->error, AdcError::NONE);
    zassert_within(a1->volts, 1.5F, 1e-6F);
}

ZTEST(adc_sample_set, test_Acquire_is_one_request_per_tick_on_the_emulator) {
    auto adc = processing_test_support::MakeEmulator();
    zassert_true(adc->Initialize());
    processing_test_support::DriveConstant(*adc, 1, 1000);
    processing_test_support::DriveConstant(*adc, 3, 2000);

    AdcSampleSet adc_samples;
    const AdcSample* slot3 = adc_samples.Reserve(3, { .adc = adc.get(), .local_channel = 3 }, 32);
    const AdcSample* slot1 = adc_samples.Reserve(1, { .adc = adc.get(), .local_channel = 1 }, 32);
    adc_samples.Seal();

    const uint32_t requests_before = adc->GetStats().requests;
    for(int tick = 0; tick < 5; tick++)
        adc_samples.Acquire();

    zassert_equal(adc->GetStats().requests - requests_before, 5);
    zassert_equal(slot1->error, AdcError::NONE);
    zassert_equal(slot3->error, AdcError::NONE);
    zassert_within(slot1->volts, 1.0F, processing_test_support::kEmulTolerance);
    zassert_within(slot3->volts, 2.0F, processing_test_support::kEmulTolerance);
}

ZTEST(adc_sample_set, test_Reserve_after_Seal_throws) {
    FakeAdc adc(4);
    AdcSampleSet adc_samples;
    (void)adc_samples.Reserve(0, { .adc = &adc, .local_channel = 0 }, 1);
    adc_samples.Seal();

    bool is_thrown = false;
    try {
        (void)adc_samples.Reserve(1, { .adc = &adc, .local_channel = 1 }, 1);
    } catch(const std::logic_error&) {
        is_thrown = true;
    }

    zassert_true(is_thrown, "The request spans are built; a new channel would never be sampled");
}

ZTEST(adc_sample_set, test_Reserve_rejects_a_handle_the_adc_cannot_serve) {
    FakeAdc adc(4);
    AdcSampleSet adc_samples;

    bool is_out_of_range_thrown = false;
    try {
        (void)adc_samples.Reserve(4, { .adc = &adc, .local_channel = 4 }, 1);
    } catch(const std::invalid_argument&) {
        is_out_of_range_thrown = true;
    }

    bool is_null_thrown = false;
    try {
        (void)adc_samples.Reserve(0, { .adc = nullptr, .local_channel = 0 }, 1);
    } catch(const std::invalid_argument&) {
        is_null_thrown = true;
    }

    zassert_true(is_out_of_range_thrown);
    zassert_true(is_null_thrown);
    zassert_equal(adc_samples.GetAdcCount(), 0, "A rejected handle reserves nothing");
}

ZTEST(adc_sample_set, test_GetAdc_reports_each_adc_as_reserved) {
    FakeAdc adc_a(4);
    FakeAdc adc_b(2);
    adc_a.volts_base = 0.5F;
    adc_a.volts_step = 0.25F;
    adc_b.volts_base = 3.0F;

    AdcSampleSet adc_samples;

    const AdcSample* a3 = adc_samples.Reserve(3, { .adc = &adc_a, .local_channel = 3 }, 8);
    (void)adc_samples.Reserve(5, { .adc = &adc_b, .local_channel = 1 }, 2);
    (void)adc_samples.Reserve(1, { .adc = &adc_a, .local_channel = 1 }, 16);

    adc_samples.Seal();
    adc_samples.Acquire();

    zassert_equal(adc_samples.GetAdcCount(), 2);

    const auto first = adc_samples.GetAdc(0);
    zassert_equal(first.adc, &adc_a, "In the order the ADCs were first reserved");
    zassert_equal(first.channels.size(), 2);
    zassert_equal(first.channels[0], 1);
    zassert_equal(first.channels[1], 3);
    zassert_equal(first.global_channels.size(), 2, "One global index per reserved channel");
    zassert_equal(first.global_channels[0], 1);
    zassert_equal(first.global_channels[1], 3);
    zassert_equal(first.samples, 16);
    zassert_equal(first.results.size(), 4, "Results are indexed by local channel");
    zassert_equal(&first.results[3], a3, "The view shows the slots the readers hold");
    zassert_within(first.results[1].volts, 0.75F, 1e-6F);
    zassert_within(first.results[3].volts, 1.25F, 1e-6F);

    const auto second = adc_samples.GetAdc(1);
    zassert_equal(second.adc, &adc_b);
    zassert_equal(second.channels.size(), 1);
    zassert_equal(second.channels[0], 1);
    zassert_equal(second.global_channels[0], 5);
    zassert_equal(second.samples, 2);
    zassert_within(second.results[1].volts, 3.0F, 1e-6F);
}

// The PM board's layout: global channel 0 is channel 6 of the first ADC, global channels 1-3 are
// channels 4-6 of the second.
ZTEST(adc_sample_set, test_GetAdc_keeps_the_global_channel_of_each_reservation) {
    FakeAdc unit_1(7);
    FakeAdc unit_2(7);

    AdcSampleSet adc_samples;
    (void)adc_samples.Reserve(3, { .adc = &unit_2, .local_channel = 6 }, 4);
    (void)adc_samples.Reserve(0, { .adc = &unit_1, .local_channel = 6 }, 4);
    (void)adc_samples.Reserve(1, { .adc = &unit_2, .local_channel = 4 }, 4);
    (void)adc_samples.Reserve(3, { .adc = &unit_2, .local_channel = 6 }, 4);
    adc_samples.Seal();

    const auto first = adc_samples.GetAdc(0);
    zassert_equal(first.adc, &unit_2);
    zassert_equal(first.channels.size(), 2);
    zassert_equal(first.channels[0], 4);
    zassert_equal(first.global_channels[0], 1, "Moved with its local channel when 4 went before 6");
    zassert_equal(first.channels[1], 6);
    zassert_equal(first.global_channels[1], 3);

    const auto second = adc_samples.GetAdc(1);
    zassert_equal(second.adc, &unit_1);
    zassert_equal(second.channels.size(), 1);
    zassert_equal(second.channels[0], 6);
    zassert_equal(second.global_channels[0], 0);
}

ZTEST(adc_sample_set, test_Reserve_rejects_one_channel_under_two_global_channels) {
    FakeAdc adc(4);
    AdcSampleSet adc_samples;
    const AdcSample* slot = adc_samples.Reserve(2, { .adc = &adc, .local_channel = 2 }, 1);

    bool is_thrown = false;
    try {
        (void)adc_samples.Reserve(5, { .adc = &adc, .local_channel = 2 }, 1);
    } catch(const std::invalid_argument&) {
        is_thrown = true;
    }

    zassert_true(is_thrown, "A local channel has one global index");
    zassert_equal(adc_samples.Reserve(2, { .adc = &adc, .local_channel = 2 }, 1), slot);
    zassert_equal(adc_samples.GetChannelCount(), 1);
    zassert_equal(adc_samples.GetAdc(0).global_channels[0], 2);
}
