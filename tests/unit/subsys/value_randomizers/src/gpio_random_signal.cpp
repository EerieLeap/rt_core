#include <zephyr/ztest.h>

#include "subsys/gpio/gpio_random_signal.h"

using namespace eerie_leap::subsys::gpio;

ZTEST_SUITE(gpio_random_signal, NULL, NULL, NULL, NULL, NULL);

ZTEST(gpio_random_signal, test_flip_intervals_stay_within_the_hold_bounds) {
    GpioRandomSignal signal({ .hold_min_ms = 100, .hold_max_ms = 300, .seed = 8 });

    bool level = signal.At(1000);
    int64_t last_flip = -1;
    int flips = 0;

    for(int64_t t = 1000; t < 200000; t++) {
        const bool now = signal.At(t);
        if(now != level) {
            if(last_flip >= 0) {
                const int64_t interval = t - last_flip;
                zassert_true(interval >= 100 && interval <= 300, "interval %lld", interval);
            }
            last_flip = t;
            level = now;
            flips++;
        }
    }

    zassert_true(flips > 500, "%d", flips);
}

ZTEST(gpio_random_signal, test_next_flip_is_exactly_when_the_level_changes) {
    GpioRandomSignal signal({ .hold_min_ms = 50, .hold_max_ms = 400, .seed = 21 });

    bool level = signal.At(0);
    for(int i = 0; i < 200; i++) {
        const int64_t flip = signal.GetNextFlipMs();

        zassert_true(flip > 0);
        zassert_equal(signal.At(flip - 1), level, "no flip before the announced time");

        level = !level;
        zassert_equal(signal.At(flip), level, "flip at the announced time");
    }
}

ZTEST(gpio_random_signal, test_first_flip_is_a_hold_after_the_first_call) {
    GpioRandomSignal signal({ .hold_min_ms = 100, .hold_max_ms = 200, .seed = 4 });

    (void)signal.At(5000);

    zassert_true(signal.GetNextFlipMs() >= 5100 && signal.GetNextFlipMs() <= 5200);
}

ZTEST(gpio_random_signal, test_same_seed_is_deterministic) {
    GpioRandomSignal a({ .hold_min_ms = 10, .hold_max_ms = 90, .seed = 77 });
    GpioRandomSignal b({ .hold_min_ms = 10, .hold_max_ms = 90, .seed = 77 });
    GpioRandomSignal c({ .hold_min_ms = 10, .hold_max_ms = 90, .seed = 78 });
    int different = 0;

    for(int64_t t = 0; t < 20000; t += 3) {
        const bool level = a.At(t);
        zassert_equal(level, b.At(t));
        different += level != c.At(t) ? 1 : 0;
    }

    zassert_true(different > 100);
}

ZTEST(gpio_random_signal, test_both_levels_occur) {
    GpioRandomSignal signal({ .hold_min_ms = 10, .hold_max_ms = 50, .seed = 2 });
    int high = 0;

    for(int64_t t = 0; t < 10000; t++)
        high += signal.At(t) ? 1 : 0;

    zassert_true(high > 3000 && high < 7000, "%d", high);
}

ZTEST(gpio_random_signal, test_a_long_gap_costs_bounded_work_and_keeps_flipping) {
    GpioRandomSignal signal({ .hold_min_ms = 1, .hold_max_ms = 2, .seed = 6 });

    (void)signal.At(0);

    const int64_t later = 365LL * 24 * 3600 * 1000;
    (void)signal.At(later);

    // The next flip is a hold after the restart, not a long way behind.
    zassert_true(signal.GetNextFlipMs() > later && signal.GetNextFlipMs() <= later + 2);
}

ZTEST(gpio_random_signal, test_zero_hold_is_raised_to_one_ms) {
    GpioRandomSignal signal({ .hold_min_ms = 0, .hold_max_ms = 0, .seed = 3 });

    (void)signal.At(100);

    zassert_equal(signal.GetNextFlipMs(), 101);
}
