// The channel part of the rate group statistics log: global channel numbers, millivolt volts,
// errors, and lines that wrap on whole channels. Only built in the processing_stats scenario.
#ifdef CONFIG_EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_STATS

#include <array>
#include <string>

#include <zephyr/ztest.h>

#include "subsys/adc/adc_types.h"
#include "domain/sensor_domain/services/adc_sample_set.h"
#include "domain/sensor_domain/services/rate_group_scheduler.h"

#include "processing_test_support.h"

using namespace eerie_leap::subsys::adc;
using namespace eerie_leap::domain::sensor_domain::services;

using processing_test_support::FakeAdc;

ZTEST_SUITE(processing_stats, NULL, NULL, NULL, NULL, NULL);

namespace {

// The PM board's split: global channel 0 is the only channel of the first ADC, global channels
// 1-3 are channels 0-2 of the second.
struct processing_stats_PmLayout {
    FakeAdc unit_1 { 1 };
    FakeAdc unit_2 { 3 };
    AdcSampleSet adc_samples;

    processing_stats_PmLayout() {
        unit_1.volts_base = 0.898F;
        unit_2.volts_base = 0.0F;
        unit_2.volts_step = 0.25F;

        (void)adc_samples.Reserve(0, { .adc = &unit_1, .local_channel = 0 }, 4);
        (void)adc_samples.Reserve(3, { .adc = &unit_2, .local_channel = 2 }, 4);
        (void)adc_samples.Reserve(1, { .adc = &unit_2, .local_channel = 0 }, 4);
        adc_samples.Seal();
        adc_samples.Acquire();
    }
};

} // namespace

ZTEST(processing_stats, test_FormatChannels_names_channels_by_their_global_index) {
    processing_stats_PmLayout layout;
    std::array<char, 128> line {};

    const size_t next = RateGroupScheduler::FormatChannels(layout.adc_samples, 0, line);

    zassert_equal(next, 3, "Every channel fits");
    zassert_str_equal(line.data(), " ch0 0.898 V ch1 0.000 V ch3 0.500 V");
}

ZTEST(processing_stats, test_FormatChannels_reports_a_failed_channel_by_errno) {
    processing_stats_PmLayout layout;
    layout.unit_2.failing_channel = 2;
    layout.adc_samples.Acquire();
    std::array<char, 128> line {};

    (void)RateGroupScheduler::FormatChannels(layout.adc_samples, 0, line);

    zassert_str_equal(line.data(), " ch0 0.898 V ch1 0.000 V ch3 err -5");
}

ZTEST(processing_stats, test_FormatChannels_rounds_to_the_millivolt) {
    FakeAdc adc(3);
    adc.volts_base = -1.25F;
    adc.volts_step = 1.6253F;

    AdcSampleSet adc_samples;
    for(uint8_t channel = 0; channel < 3; channel++)
        (void)adc_samples.Reserve(channel, { .adc = &adc, .local_channel = channel }, 1);
    adc_samples.Seal();
    adc_samples.Acquire();

    std::array<char, 128> line {};
    (void)RateGroupScheduler::FormatChannels(adc_samples, 0, line);

    zassert_str_equal(line.data(), " ch0 -1.250 V ch1 0.375 V ch2 2.001 V");
}

ZTEST(processing_stats, test_FormatChannels_wraps_on_whole_channels) {
    processing_stats_PmLayout layout;

    // " ch0 0.898 V" is 12 characters: two do not fit with the terminator, one does.
    std::array<char, 24> line {};
    std::string joined;
    size_t lines = 0;

    for(size_t next = 0; next < layout.adc_samples.GetChannelCount();) {
        const size_t after = RateGroupScheduler::FormatChannels(layout.adc_samples, next, line);
        zassert_true(after > next, "Every line holds at least one channel");

        joined += line.data();
        next = after;
        lines++;
    }

    zassert_equal(lines, 3);
    zassert_equal(joined, " ch0 0.898 V ch1 0.000 V ch3 0.500 V", "Nothing lost between lines");
}

ZTEST(processing_stats, test_FormatChannels_cuts_a_channel_the_line_cannot_hold) {
    processing_stats_PmLayout layout;
    std::array<char, 8> line {};

    const size_t next = RateGroupScheduler::FormatChannels(layout.adc_samples, 0, line);

    zassert_equal(next, 1, "The cut channel still counts as written");
    zassert_str_equal(line.data(), " ch0 0.");
}

ZTEST(processing_stats, test_FormatChannels_of_a_set_without_channels_is_empty) {
    AdcSampleSet adc_samples;
    adc_samples.Seal();
    std::array<char, 16> line;
    line.fill('x');

    zassert_equal(RateGroupScheduler::FormatChannels(adc_samples, 0, line), 0);
    zassert_str_equal(line.data(), "");
}

#endif
