#include <cstdint>
#include <cstdlib>
#include <zephyr/ztest.h>

#include "utilities/math/ema_filter.hpp"

using eerie_leap::utilities::math::EmaFilter;

namespace {

template<typename T>
T RunSteps(EmaFilter<T>& filter, T input, uint8_t k2, int steps) {
    T last = filter.Get();
    for (int i = 0; i < steps; ++i)
        last = filter.Filter(input, k2);

    return last;
}

} // namespace

ZTEST_SUITE(ema_filter, NULL, NULL, NULL, NULL, NULL);

ZTEST(ema_filter, test_Get_returns_seeded_value) {
    EmaFilter<int32_t> filter(1234);

    zassert_equal(filter.Get(), 1234);
}

ZTEST(ema_filter, test_Filter_returns_new_state) {
    EmaFilter<int32_t> filter(0);

    zassert_equal(filter.Filter(100, 2), filter.Get());
}

ZTEST(ema_filter, test_k2_zero_is_passthrough) {
    EmaFilter<int32_t> filter(0);

    zassert_equal(filter.Filter(100, 0), 100);
    zassert_equal(filter.Filter(-50, 0), -50);
    zassert_equal(filter.Get(), -50);
}

ZTEST(ema_filter, test_step_response_known_values) {
    EmaFilter<int32_t> filter(0);

    zassert_equal(filter.Filter(100, 2), 25);
    zassert_equal(filter.Filter(100, 2), 43);
    zassert_equal(filter.Filter(100, 2), 58);
    zassert_equal(filter.Filter(100, 2), 68);
}

ZTEST(ema_filter, test_converges_upward_exactly) {
    EmaFilter<int32_t> filter(0);

    zassert_equal(RunSteps(filter, 100, 2, 64), 100);
    zassert_equal(RunSteps(filter, 100, 2, 16), 100);
}

ZTEST(ema_filter, test_converges_downward_exactly) {
    EmaFilter<int32_t> filter(100);

    zassert_equal(RunSteps(filter, 0, 2, 64), 0);
    zassert_equal(RunSteps(filter, 0, 2, 16), 0);
}

ZTEST(ema_filter, test_converges_to_negative_target) {
    EmaFilter<int32_t> filter(0);

    zassert_equal(RunSteps(filter, -100, 3, 256), -100);
}

ZTEST(ema_filter, test_residual_removes_steady_state_error) {
    // A step smaller than 2^k2 shifts out to zero; only the accumulated
    // residual lets the filter reach the target.
    EmaFilter<int32_t> filter(0);

    zassert_equal(filter.Filter(1, 4), 0);
    zassert_equal(RunSteps(filter, 1, 4, 15), 1);
    zassert_equal(RunSteps(filter, 1, 4, 8), 1);
}

ZTEST(ema_filter, test_step_response_never_overshoots) {
    EmaFilter<int32_t> filter(0);

    int32_t previous = 0;
    for (int i = 0; i < 128; ++i) {
        int32_t current = filter.Filter(1000, 3);

        zassert_true(current >= previous, "filter went backwards at step %d", i);
        zassert_true(current <= 1000, "filter overshot at step %d", i);

        previous = current;
    }

    zassert_equal(previous, 1000);
}

ZTEST(ema_filter, test_larger_k2_reacts_slower) {
    EmaFilter<int32_t> fast(0);
    EmaFilter<int32_t> slow(0);

    int32_t fast_value = RunSteps(fast, 1000, 1, 3);
    int32_t slow_value = RunSteps(slow, 1000, 5, 3);

    zassert_true(fast_value > slow_value);
    zassert_equal(fast_value, 875);
    zassert_equal(slow_value, 90);
}

ZTEST(ema_filter, test_smooths_alternating_input) {
    EmaFilter<int32_t> filter(0);

    for (int i = 0; i < 200; ++i) {
        int32_t value = filter.Filter((i % 2 == 0) ? 100 : -100, 3);

        zassert_between_inclusive(value, -25, 25, "unsmoothed output %d at step %d", value, i);
    }
}

ZTEST(ema_filter, test_seeded_value_avoids_ramp_up) {
    EmaFilter<int32_t> seeded(100);

    zassert_equal(seeded.Filter(100, 4), 100);
    zassert_equal(seeded.Get(), 100);
}

ZTEST(ema_filter, test_int16_type) {
    EmaFilter<int16_t> filter(0);

    zassert_equal(RunSteps<int16_t>(filter, 1000, 3, 128), 1000);
    zassert_equal(RunSteps<int16_t>(filter, -1000, 3, 128), -1000);
}

ZTEST(ema_filter, test_int64_type) {
    EmaFilter<int64_t> filter(0);

    zassert_equal(RunSteps<int64_t>(filter, 1000000000000LL, 4, 512), 1000000000000LL);
}
