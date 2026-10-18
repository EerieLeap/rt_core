#include <stdexcept>

#include <zephyr/ztest.h>

#include "subsys/smp/can/smp_can_id_layout.h"

using eerie_leap::subsys::smp::can::SmpCanIdLayout;

ZTEST_SUITE(smp_can_id_layout, NULL, NULL, NULL, NULL, NULL);

ZTEST(smp_can_id_layout, test_id_carries_target_and_source) {
    constexpr SmpCanIdLayout LAYOUT;

    constexpr uint32_t ID = LAYOUT.MakeId(0x07, 0x03);

    zassert_equal(ID, 0x1FF00703);
    zassert_equal(SmpCanIdLayout::GetTarget(ID), 0x07);
    zassert_equal(SmpCanIdLayout::GetSource(ID), 0x03);
    zassert_true(LAYOUT.Contains(ID));
    zassert_false(LAYOUT.Contains(0x1FE00703));
}

ZTEST(smp_can_id_layout, test_target_filter_accepts_any_source) {
    constexpr SmpCanIdLayout LAYOUT(0x1E000000);
    constexpr uint32_t FILTER_ID = LAYOUT.GetTargetFilterId(0x07);

    const auto matches = [&](uint32_t id) {
        return (id & SmpCanIdLayout::TARGET_FILTER_MASK) == FILTER_ID;
    };

    zassert_true(matches(LAYOUT.MakeId(0x07, 0x01)));
    zassert_true(matches(LAYOUT.MakeId(0x07, 0xFE)));
    zassert_false(matches(LAYOUT.MakeId(0x08, 0x01)));
    zassert_false(matches(SmpCanIdLayout().MakeId(0x07, 0x01)));
}

ZTEST(smp_can_id_layout, test_base_must_be_29_bit_with_clear_address_bits) {
    zassert_true(SmpCanIdLayout::IsValidBase(SmpCanIdLayout::DEFAULT_BASE));
    zassert_true(SmpCanIdLayout::IsValidBase(0x00010000));
    zassert_false(SmpCanIdLayout::IsValidBase(0x1FF00100));
    zassert_false(SmpCanIdLayout::IsValidBase(0x20000000));

    bool threw = false;
    try {
        SmpCanIdLayout layout(0x1FF00001);
        (void)layout;
    } catch(const std::invalid_argument&) {
        threw = true;
    }

    zassert_true(threw);
}

ZTEST(smp_can_id_layout, test_unassigned_and_broadcast_are_not_addresses) {
    zassert_false(SmpCanIdLayout::IsValidAddress(0x00));
    zassert_false(SmpCanIdLayout::IsValidAddress(0xFF));
    zassert_true(SmpCanIdLayout::IsValidAddress(0x01));
    zassert_true(SmpCanIdLayout::IsValidAddress(0xFE));
}
