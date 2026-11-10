#include <cmath>

#include <zephyr/ztest.h>

#include "subsys/adc/adc_random_signal.h"

using namespace eerie_leap::subsys::adc;

namespace {

AdcRandomSignalConfig Config(uint32_t seed = 42) {
    return { .volts_max = 3.3F, .move_min_ms = 200, .move_max_ms = 1000, .hold_max_ms = 500, .seed = seed };
}

} // namespace

ZTEST_SUITE(adc_random_signal, NULL, NULL, NULL, NULL, NULL);

ZTEST(adc_random_signal, test_stays_within_zero_and_volts_max) {
    AdcRandomSignal signal(Config());

    for(int64_t t = 0; t < 120000; t += 7) {
        const float v = signal.At(t);
        zassert_true(v >= 0.0F && v <= 3.3F, "t=%lld v=%f", t, static_cast<double>(v));
    }
}

ZTEST(adc_random_signal, test_is_continuous) {
    AdcRandomSignal signal(Config());

    // The fastest possible move covers at most 3.3 V in 200 ms; a 1 ms step may change by the
    // smoothstep's peak slope of 1.5 times that average.
    const float max_step = 3.3F / 200.0F * 1.5F * 1.01F;

    float last = signal.At(0);
    for(int64_t t = 1; t < 60000; t++) {
        const float v = signal.At(t);
        zassert_true(std::fabs(v - last) <= max_step, "t=%lld jump %f", t, static_cast<double>(v - last));
        last = v;
    }
}

ZTEST(adc_random_signal, test_moves_and_holds) {
    AdcRandomSignal signal(Config());

    int moving = 0;
    int still = 0;
    float last = signal.At(0);
    for(int64_t t = 1; t < 60000; t++) {
        const float v = signal.At(t);
        if(v == last)
            still++;
        else
            moving++;
        last = v;
    }

    zassert_true(moving > 1000, "moving %d", moving);
    zassert_true(still > 1000, "still %d", still);
}

ZTEST(adc_random_signal, test_covers_the_range_over_time) {
    AdcRandomSignal signal(Config());
    float min = 99.0F;
    float max = -1.0F;

    for(int64_t t = 0; t < 600000; t += 10) {
        const float v = signal.At(t);
        min = v < min ? v : min;
        max = v > max ? v : max;
    }

    zassert_true(min < 0.4F, "%f", static_cast<double>(min));
    zassert_true(max > 2.9F, "%f", static_cast<double>(max));
}

ZTEST(adc_random_signal, test_same_seed_same_signal_other_seed_differs) {
    AdcRandomSignal a(Config(5));
    AdcRandomSignal b(Config(5));
    AdcRandomSignal c(Config(6));
    int different = 0;

    for(int64_t t = 0; t < 20000; t += 13) {
        const float va = a.At(t);
        zassert_equal(va, b.At(t));
        different += va != c.At(t) ? 1 : 0;
    }

    zassert_true(different > 100);
}

ZTEST(adc_random_signal, test_move_times_follow_the_configured_bounds) {
    // Without a hold and with a fixed move time the signal is always moving, and the value is a
    // smoothstep between two targets: it reaches each target exactly at every move_ms.
    AdcRandomSignal signal({ .volts_max = 3.3F, .move_min_ms = 500, .move_max_ms = 500, .hold_max_ms = 0, .seed = 3 });

    const float start = signal.At(0);
    const float mid = signal.At(250);
    const float end = signal.At(500);
    const float next_mid = signal.At(750);
    const float next_end = signal.At(1000);

    // Halfway through a smoothstep is the midpoint of the move.
    zassert_within(mid, (start + end) / 2.0F, 0.001F);
    zassert_within(next_mid, (end + next_end) / 2.0F, 0.001F);
}

ZTEST(adc_random_signal, test_hold_is_flat_for_at_most_hold_max) {
    AdcRandomSignal signal(
        { .volts_max = 3.3F, .move_min_ms = 100, .move_max_ms = 100, .hold_max_ms = 300, .seed = 11 }
    );

    // Longest run of equal values, in ms, must not exceed hold_max (+1 for the sample that ends it).
    int64_t run_start = 0;
    int64_t longest = 0;
    float last = signal.At(0);
    for(int64_t t = 1; t < 120000; t++) {
        const float v = signal.At(t);
        if(v != last) {
            longest = (t - run_start) > longest ? (t - run_start) : longest;
            run_start = t;
        }
        last = v;
    }

    zassert_true(longest <= 301, "%lld", longest);
    zassert_true(longest >= 100, "%lld", longest);
}

ZTEST(adc_random_signal, test_a_long_gap_restarts_from_the_current_target) {
    AdcRandomSignal signal(Config());

    (void)signal.At(0);

    // A year later: bounded work, in range, and moving again.
    const int64_t later = 365LL * 24 * 3600 * 1000;
    const float v = signal.At(later);
    zassert_true(v >= 0.0F && v <= 3.3F);

    int changed = 0;
    float last = v;
    for(int64_t t = later + 1; t < later + 20000; t++) {
        const float next = signal.At(t);
        changed += next != last ? 1 : 0;
        last = next;
    }

    zassert_true(changed > 100);
}

ZTEST(adc_random_signal, test_a_gap_is_continuous_with_the_target_it_restarts_from) {
    AdcRandomSignal signal({ .volts_max = 3.3F, .move_min_ms = 100, .move_max_ms = 100, .hold_max_ms = 0, .seed = 9 });

    (void)signal.At(0);
    const float after_gap = signal.At(1000000);
    const float soon = signal.At(1000001);

    zassert_within(after_gap, soon, 0.1F);
}

ZTEST(adc_random_signal, test_time_going_back_does_not_break_the_signal) {
    AdcRandomSignal signal(Config());

    (void)signal.At(5000);
    const float v = signal.At(100);

    zassert_true(v >= 0.0F && v <= 3.3F);
}

ZTEST(adc_random_signal, test_degenerate_config_is_tolerated) {
    // max below min, zero volts: no division by zero, no NaN.
    AdcRandomSignal inverted(
        { .volts_max = 1.0F, .move_min_ms = 900, .move_max_ms = 100, .hold_max_ms = 0, .seed = 1 }
    );
    AdcRandomSignal zero_move({ .volts_max = 1.0F, .move_min_ms = 0, .move_max_ms = 0, .hold_max_ms = 0, .seed = 1 });
    AdcRandomSignal flat({ .volts_max = 0.0F, .move_min_ms = 100, .move_max_ms = 200, .hold_max_ms = 10, .seed = 1 });

    for(int64_t t = 0; t < 5000; t += 3) {
        zassert_true(!std::isnan(inverted.At(t)));
        zassert_true(!std::isnan(zero_move.At(t)));
        zassert_equal(flat.At(t), 0.0F);
    }
}
