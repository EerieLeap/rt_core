#include <cstdint>

#include <zephyr/ztest.h>

#include "subsys/canbus/can_id.h"
#include "subsys/canbus/can_filter.h"

using eerie_leap::subsys::canbus::CanFilter;
using eerie_leap::subsys::canbus::CanId;

ZTEST_SUITE(can_id, NULL, NULL, NULL, NULL, NULL);

ZTEST(can_id, test_factories_set_the_format) {
    zassert_false(CanId::Standard(0x123).is_extended);
    zassert_true(CanId::Extended(0x123).is_extended);
    zassert_equal(CanId::Extended(0x123).id, 0x123);
}

ZTEST(can_id, test_validity_follows_the_format_width) {
    zassert_true(CanId::Standard(0x7FF).IsValid());
    zassert_false(CanId::Standard(0x800).IsValid());
    zassert_true(CanId::Extended(0x1FFFFFFF).IsValid());
    zassert_false(CanId::Extended(0x20000000).IsValid());
}

ZTEST(can_id, test_key_distinguishes_formats) {
    zassert_not_equal(CanId::Standard(0x123).Key(), CanId::Extended(0x123).Key());
    zassert_equal(CanId::Extended(0x123).Key(), CanId::Extended(0x123).Key());
    zassert_not_equal(CanId::Standard(0x123), CanId::Extended(0x123));
}

ZTEST_SUITE(can_filter, NULL, NULL, NULL, NULL, NULL);

ZTEST(can_filter, test_id_converts_to_an_exact_filter) {
    const CanFilter standard = CanId::Standard(0x123);
    const CanFilter extended = CanId::Extended(0x123);

    zassert_equal(standard.id, 0x123);
    zassert_false(standard.is_extended);
    zassert_equal(standard.mask, 0x7FF);
    zassert_true(extended.is_extended);
    zassert_equal(extended.mask, 0x1FFFFFFF);
    zassert_true(standard.IsExact());
    zassert_true(extended.IsExact());
    zassert_false(CanFilter::Masked(CanId::Extended(0x123), 0x1FFFFF00).IsExact());
}

ZTEST(can_filter, test_masked_filter_matches_one_format_only) {
    const CanFilter filter = CanFilter::Masked(CanId::Extended(0x1FF00700), 0x1FFFFF00);

    zassert_true(filter.Matches(CanId::Extended(0x1FF00700)));
    zassert_true(filter.Matches(CanId::Extended(0x1FF007AB)));
    zassert_false(filter.Matches(CanId::Extended(0x1FF008AB)));
    zassert_false(filter.Matches(CanId::Standard(0x700)));
}

ZTEST(can_filter, test_validity_covers_id_and_mask) {
    zassert_true(CanFilter::Masked(CanId::Standard(0x700), 0x700).IsValid());
    zassert_false(CanFilter::Masked(CanId::Standard(0x700), 0xF00).IsValid());
    zassert_false(CanFilter::Masked(CanId::Standard(0x900), 0x700).IsValid());
}

ZTEST(can_filter, test_normalization_ignores_bits_outside_the_mask) {
    const CanFilter a = CanFilter::Masked(CanId::Extended(0x1FF007AB), 0x1FFFFF00);
    const CanFilter b = CanFilter::Masked(CanId::Extended(0x1FF00700), 0x1FFFFF00);

    zassert_not_equal(a, b);
    zassert_equal(a.Normalized(), b.Normalized());
}

ZTEST(can_filter, test_overlap_detection) {
    const CanFilter target_7 = CanFilter::Masked(CanId::Extended(0x1FF00700), 0x1FFFFF00);
    const CanFilter target_8 = CanFilter::Masked(CanId::Extended(0x1FF00800), 0x1FFFFF00);
    const CanFilter wide = CanFilter::Masked(CanId::Extended(0x1FF00000), 0x1FF00000);

    zassert_true(target_7.Overlaps(CanId::Extended(0x1FF00705)));
    zassert_false(target_7.Overlaps(CanId::Extended(0x1FF00805)));
    zassert_false(target_7.Overlaps(target_8));
    zassert_true(target_7.Overlaps(wide));
    zassert_true(wide.Overlaps(target_7), "Overlap must be symmetric");
    zassert_false(target_7.Overlaps(CanFilter::Masked(CanId::Standard(0x700), 0x700)),
        "Filters of different formats never overlap");
    zassert_false(CanFilter(CanId::Standard(0x100)).Overlaps(CanId::Standard(0x101)));
    zassert_true(CanFilter(CanId::Standard(0x100)).Overlaps(CanId::Standard(0x100)));
}
