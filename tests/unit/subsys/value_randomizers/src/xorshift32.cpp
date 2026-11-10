#include <zephyr/ztest.h>

#include "utilities/math/xorshift32.hpp"

using eerie_leap::utilities::math::Xorshift32;

ZTEST_SUITE(xorshift32, NULL, NULL, NULL, NULL, NULL);

ZTEST(xorshift32, test_same_seed_gives_the_same_sequence) {
    Xorshift32 a(1234);
    Xorshift32 b(1234);

    for(int i = 0; i < 100; i++)
        zassert_equal(a.Next(), b.Next());
}

ZTEST(xorshift32, test_different_seeds_give_different_sequences) {
    Xorshift32 a(1);
    Xorshift32 b(2);

    int equal = 0;
    for(int i = 0; i < 100; i++)
        equal += a.Next() == b.Next() ? 1 : 0;

    zassert_true(equal < 3);
}

ZTEST(xorshift32, test_known_first_value_for_seed_1) {
    // x ^= x << 13; x ^= x >> 17; x ^= x << 5 from 1.
    Xorshift32 rng(1);

    zassert_equal(rng.Next(), 270369U);
}

ZTEST(xorshift32, test_zero_seed_is_replaced_and_keeps_moving) {
    Xorshift32 rng(0);

    const uint32_t first = rng.Next();
    const uint32_t second = rng.Next();

    zassert_not_equal(first, 0U);
    zassert_not_equal(second, 0U);
    zassert_not_equal(first, second);
}

ZTEST(xorshift32, test_float_stays_in_unit_interval_and_spreads) {
    Xorshift32 rng(99);
    float min = 1.0F;
    float max = 0.0F;

    for(int i = 0; i < 5000; i++) {
        const float v = rng.NextFloat();
        zassert_true(v >= 0.0F && v < 1.0F, "%f", static_cast<double>(v));
        min = v < min ? v : min;
        max = v > max ? v : max;
    }

    zassert_true(min < 0.05F);
    zassert_true(max > 0.95F);
}

ZTEST(xorshift32, test_range_includes_both_ends_and_stays_inside) {
    Xorshift32 rng(7);
    bool saw_min = false;
    bool saw_max = false;

    for(int i = 0; i < 2000; i++) {
        const uint32_t v = rng.NextInRange(10, 14);
        zassert_true(v >= 10 && v <= 14, "%u", v);
        saw_min = saw_min || v == 10;
        saw_max = saw_max || v == 14;
    }

    zassert_true(saw_min);
    zassert_true(saw_max);
}

ZTEST(xorshift32, test_degenerate_and_full_ranges) {
    Xorshift32 rng(5);

    zassert_equal(rng.NextInRange(8, 8), 8U);
    zassert_equal(rng.NextInRange(9, 3), 9U);

    // The full span overflows 32 bits; it must not wrap into a division by zero.
    for(int i = 0; i < 10; i++)
        (void)rng.NextInRange(0, UINT32_MAX);
}
