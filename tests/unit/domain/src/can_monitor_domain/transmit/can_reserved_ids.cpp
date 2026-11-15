#include <zephyr/ztest.h>

#include <cstdint>
#include <optional>

#include "domain/can_monitor_domain/transmit/can_reserved_ids.h"

using namespace eerie_leap::domain::can_monitor_domain::transmit;
using eerie_leap::domain::canbus_domain::models::CanbusComConfiguration;

ZTEST_SUITE(can_reserved_ids, NULL, NULL, NULL, NULL, NULL);

namespace {

constexpr uint8_t COM_PORT = 1;

CanbusComConfiguration ComOn(uint8_t channel, uint32_t cdmp_base = 0x700, uint32_t smp_base = 0x1FF00000) {
    CanbusComConfiguration com;
    com.bus_channel = channel;
    com.cdmp_base_can_id = cdmp_base;
    com.smp_can_id_base = smp_base;

    return com;
}

} // namespace

ZTEST(can_reserved_ids, test_NothingIsReservedOffTheComPort) {
    const CanbusComConfiguration com = ComOn(COM_PORT);

    for(const uint8_t port : { uint8_t{ 0 }, uint8_t{ 2 }, uint8_t{ 255 } }) {
        const CanReservedIds ids(port, com);

        zassert_true(ids.Empty());
        zassert_equal(ids.Ranges().size(), 0U);
        zassert_false(ids.IsReserved(0x700, false));
        zassert_false(ids.IsReserved(0x763, false));
        zassert_false(ids.IsReserved(0x1FF00000, true));
        zassert_false(ids.IsReserved(0x1FF0FFFF, true));
    }
}

ZTEST(can_reserved_ids, test_NothingIsReservedWithoutAComChannel) {
    CanbusComConfiguration com;
    zassert_false(com.bus_channel.has_value());

    for(const uint8_t port : { uint8_t{ 0 }, uint8_t{ 1 } }) {
        const CanReservedIds ids(port, com);

        zassert_true(ids.Empty());
        zassert_false(ids.IsReserved(0x700, false));
        zassert_false(ids.IsReserved(0x1FF00000, true));
    }

    const CanReservedIds none;
    zassert_true(none.Empty());
    zassert_false(none.IsReserved(0x700, false));
}

ZTEST(can_reserved_ids, test_BothRangesAreReservedOnTheComPort) {
    const CanReservedIds ids(COM_PORT, ComOn(COM_PORT));

    zassert_false(ids.Empty());
    zassert_equal(ids.Ranges().size(), 2U);

    // The 11-bit CDMP range, then the 29-bit SMP one: the order ARM answers them in.
    zassert_equal(ids.Ranges()[0].from, 0x700U);
    zassert_equal(ids.Ranges()[0].to, 0x763U);
    zassert_false(ids.Ranges()[0].ext);
    zassert_equal(ids.Ranges()[1].from, 0x1FF00000U);
    zassert_equal(ids.Ranges()[1].to, 0x1FF0FFFFU);
    zassert_true(ids.Ranges()[1].ext);

    zassert_true(ids.IsReserved(0x710, false));
    zassert_true(ids.IsReserved(0x1FF01234, true));
}

ZTEST(can_reserved_ids, test_TheEdgesOfTheCdmpRange) {
    const CanReservedIds ids(COM_PORT, ComOn(COM_PORT, 0x700));

    zassert_false(ids.IsReserved(0x6FF, false), "base - 1");
    zassert_true(ids.IsReserved(0x700, false), "base");
    zassert_true(ids.IsReserved(0x701, false));
    zassert_true(ids.IsReserved(0x763, false), "base + 99");
    zassert_false(ids.IsReserved(0x764, false), "base + 100");
    zassert_false(ids.IsReserved(0x7FF, false));
    zassert_false(ids.IsReserved(0x000, false));
}

ZTEST(can_reserved_ids, test_TheEdgesOfTheSmpRange) {
    const CanReservedIds ids(COM_PORT, ComOn(COM_PORT, 0x700, 0x1FF00000));

    zassert_false(ids.IsReserved(0x1FEFFFFF, true), "base - 1");
    zassert_true(ids.IsReserved(0x1FF00000, true), "base");
    zassert_true(ids.IsReserved(0x1FF00001, true));
    zassert_true(ids.IsReserved(0x1FF0FFFE, true));
    zassert_true(ids.IsReserved(0x1FF0FFFF, true), "base | 0xFFFF");
    zassert_false(ids.IsReserved(0x1FF10000, true), "base | 0xFFFF + 1");
    zassert_false(ids.IsReserved(0x1FFFFFFF, true));
}

ZTEST(can_reserved_ids, test_TheRangesFollowTheStoredBases) {
    const CanReservedIds ids(COM_PORT, ComOn(COM_PORT, 0x100, 0x10120000));

    zassert_equal(ids.Ranges()[0].from, 0x100U);
    zassert_equal(ids.Ranges()[0].to, 0x163U);
    zassert_equal(ids.Ranges()[1].from, 0x10120000U);
    zassert_equal(ids.Ranges()[1].to, 0x1012FFFFU);

    zassert_false(ids.IsReserved(0x700, false), "The default base is free now");
    zassert_false(ids.IsReserved(0x1FF00000, true));
    zassert_true(ids.IsReserved(0x163, false));
    zassert_false(ids.IsReserved(0x164, false));
    zassert_true(ids.IsReserved(0x10120000, true));
    zassert_false(ids.IsReserved(0x1011FFFF, true));
}

ZTEST(can_reserved_ids, test_TheFormatOfTheIdentifierMatters) {
    // Bases that make the same numbers fall into the other format's range.
    const CanReservedIds ids(COM_PORT, ComOn(COM_PORT, 0x100, 0x00000000));

    // 0x400 is outside the CDMP range 0x100..0x163 and inside the SMP range 0..0xFFFF.
    zassert_false(ids.IsReserved(0x400, false));
    zassert_true(ids.IsReserved(0x400, true));

    // 0x120 is inside the CDMP range and, as a 29-bit one, inside the SMP range too.
    zassert_true(ids.IsReserved(0x120, false));
    zassert_true(ids.IsReserved(0x120, true));

    // The 29-bit range stops at 0xFFFF whatever the 11-bit one does.
    zassert_true(ids.IsReserved(0xFFFF, true));
    zassert_false(ids.IsReserved(0x10000, true));

    // And the default bases: a 29-bit 0x700 is not the CDMP base.
    const CanReservedIds defaults(COM_PORT, ComOn(COM_PORT));
    zassert_true(defaults.IsReserved(0x700, false));
    zassert_false(defaults.IsReserved(0x700, true));
}

ZTEST(can_reserved_ids, test_ARangeStaysInsideItsFormat) {
    // A base the validator would refuse (it leaves no room for +99) still gives a range of real IDs.
    const CanReservedIds ids(COM_PORT, ComOn(COM_PORT, 0x7F0));

    zassert_equal(ids.Ranges()[0].from, 0x7F0U);
    zassert_equal(ids.Ranges()[0].to, 0x7FFU);
    zassert_true(ids.IsReserved(0x7FF, false));
}

ZTEST(can_reserved_ids, test_ARangeKnowsWhatItContains) {
    const CanReservedRange range = { 10, 20, false };

    zassert_true(range.Contains(10, false));
    zassert_true(range.Contains(20, false));
    zassert_false(range.Contains(9, false));
    zassert_false(range.Contains(21, false));
    zassert_false(range.Contains(15, true));
}
