#include <cerrno>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "subsys/adc/adc_randomizer.h"

using namespace eerie_leap::subsys::adc;

namespace {

AdcRandomizerConfig Config(int channels = 4) {
    return { .channel_count = channels,
        .volts_max = 3.0F,
        .move_min_ms = 100,
        .move_max_ms = 400,
        .hold_max_ms = 200,
        .seed = 17 };
}

} // namespace

ZTEST_SUITE(adc_randomizer, NULL, NULL, NULL, NULL, NULL);

ZTEST(adc_randomizer, test_reports_channels_and_capabilities) {
    AdcRandomizer adc(Config(6));

    zassert_true(adc.Initialize());
    zassert_equal(adc.GetChannelCount(), 6);

    const auto capabilities = adc.GetCapabilities();
    zassert_true(capabilities.burst);
    zassert_true(capabilities.multi_channel);
    zassert_equal(capabilities.us_per_conversion, 0U);
}

ZTEST(adc_randomizer, test_sample_returns_volts_in_range_that_move_over_time) {
    AdcRandomizer adc(Config());
    adc.Initialize();

    const uint8_t channels[4] = { 0, 1, 2, 3 };
    AdcSample results[4];
    const AdcSampleRequest request{ .channels = channels, .results = results, .samples = 8 };

    float first[4];
    int changed = 0;

    for(int round = 0; round < 40; round++) {
        adc.Sample(request);

        for(int i = 0; i < 4; i++) {
            zassert_equal(results[i].error, AdcError::NONE);
            zassert_equal(results[i].errno_value, 0);
            zassert_true(results[i].volts >= 0.0F && results[i].volts <= 3.0F);

            if(round == 0)
                first[i] = results[i].volts;
            else if(results[i].volts != first[i])
                changed++;
        }

        k_sleep(K_MSEC(50));
    }

    zassert_true(changed > 20, "%d", changed);
}

ZTEST(adc_randomizer, test_channels_are_not_copies_of_each_other) {
    AdcRandomizer adc(Config());
    adc.Initialize();

    const uint8_t channels[3] = { 0, 1, 2 };
    AdcSample results[3];
    const AdcSampleRequest request{ .channels = channels, .results = results, .samples = 1 };

    int different = 0;
    for(int round = 0; round < 20; round++) {
        adc.Sample(request);
        different += (results[0].volts != results[1].volts && results[1].volts != results[2].volts) ? 1 : 0;
        k_sleep(K_MSEC(100));
    }

    zassert_true(different > 10);
}

ZTEST(adc_randomizer, test_same_seed_starts_the_same_way) {
    AdcRandomizer a(Config());
    AdcRandomizer b(Config());

    const uint8_t channels[1] = { 2 };
    AdcSample ra[1];
    AdcSample rb[1];

    // Both read at the same simulated time on native_sim; only the first read is compared, which
    // is the initial voltage of the channel and does not depend on the clock.
    a.Sample({ .channels = channels, .results = ra, .samples = 1 });
    b.Sample({ .channels = channels, .results = rb, .samples = 1 });

    zassert_equal(ra[0].volts, rb[0].volts);
}

ZTEST(adc_randomizer, test_out_of_range_and_descending_channels_are_marked) {
    AdcRandomizer adc(Config(3));
    adc.Initialize();

    const uint8_t channels[5] = { 0, 3, 1, 1, 2 };
    AdcSample results[5];
    adc.Sample({ .channels = channels, .results = results, .samples = 1 });

    zassert_equal(results[0].error, AdcError::NONE);
    zassert_equal(results[1].error, AdcError::CHANNEL_OUT_OF_RANGE);
    zassert_equal(results[1].errno_value, -EINVAL);
    zassert_equal(results[1].volts, 0.0F);
    // After the rejected 3, 1 is still above the last accepted channel 0.
    zassert_equal(results[2].error, AdcError::NONE);
    // 1 again is not ascending.
    zassert_equal(results[3].error, AdcError::CHANNEL_OUT_OF_RANGE);
    zassert_equal(results[4].error, AdcError::NONE);
}

ZTEST(adc_randomizer, test_stats_count_like_the_simulator) {
    AdcRandomizer adc(Config(2));
    adc.Initialize();

    const uint8_t channels[3] = { 0, 1, 2 };
    AdcSample results[3];
    adc.Sample({ .channels = channels, .results = results, .samples = 1 });
    adc.Sample({ .channels = channels, .results = results, .samples = 50 });

    const auto stats = adc.GetStats();
    zassert_equal(stats.requests, 2U);
    zassert_equal(stats.conversions, 4U);
    zassert_equal(stats.errors, 2U);
    zassert_equal(stats.driver_calls, 0U);
    zassert_equal(stats.timeouts, 0U);
}

ZTEST(adc_randomizer, test_a_short_results_span_limits_the_request) {
    AdcRandomizer adc(Config());
    adc.Initialize();

    const uint8_t channels[3] = { 0, 1, 2 };
    AdcSample results[2];
    adc.Sample({ .channels = channels, .results = results, .samples = 1 });

    zassert_equal(adc.GetStats().conversions, 2U);
}

ZTEST(adc_randomizer, test_zero_channels_rejects_every_request) {
    AdcRandomizer adc(Config(0));
    adc.Initialize();

    const uint8_t channels[1] = { 0 };
    AdcSample results[1];
    adc.Sample({ .channels = channels, .results = results, .samples = 1 });

    zassert_equal(results[0].error, AdcError::CHANNEL_OUT_OF_RANGE);
}
