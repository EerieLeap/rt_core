#include <zephyr/ztest.h>

#include <array>
#include <cstdint>

#include "can_monitor_test_utils.h"
#include "domain/can_monitor_domain/utilities/can_bit_time.h"

using namespace eerie_leap::domain::can_monitor_domain::utilities;
using namespace can_monitor_test;

ZTEST_SUITE(can_monitor_bit_time, NULL, NULL, NULL, NULL, NULL);

namespace {

constexpr uint32_t NOMINAL = 500000;
constexpr uint32_t DATA = 2000000;

CanBitTimeResult Classic(bool ide, bool rtr, size_t n) {
    return CanBitTime::Compute(ide, false, false, rtr, n, NOMINAL, 0);
}

CanBitTimeResult Fd(bool ide, bool brs, size_t n) {
    return CanBitTime::Compute(ide, true, brs, false, n, NOMINAL, DATA);
}

} // namespace

ZTEST(can_monitor_bit_time, test_ClassicFramesTake47Or67Plus8n) {
    for(size_t n = 0; n <= 8; ++n) {
        const auto standard = Classic(false, false, n);
        zassert_equal(standard.nominal_bits, 47U + (8U * n));
        zassert_equal(standard.data_bits, 0U);

        const auto extended = Classic(true, false, n);
        zassert_equal(extended.nominal_bits, 67U + (8U * n));
        zassert_equal(extended.data_bits, 0U);
    }

    // 111 bits at 500 kbit/s: 222 µs.
    zassert_equal(Classic(false, false, 8).time_ns, 222000U);
    zassert_equal(Classic(true, false, 8).nominal_bits, 131U);
    zassert_equal(Classic(true, false, 8).time_ns, 262000U);
}

ZTEST(can_monitor_bit_time, test_ARemoteFrameHasNoDataField) {
    zassert_equal(Classic(false, true, 8).nominal_bits, 47U);
    zassert_equal(Classic(true, true, 8).nominal_bits, 67U);
    zassert_equal(CanBitTime::Compute(false, false, false, true, 4, NOMINAL, 0).nominal_bits, 47U);
    zassert_equal(Classic(false, true, 0).time_ns, 94000U);
}

ZTEST(can_monitor_bit_time, test_FdWithoutBrsRunsAtTheNominalBitrate) {
    // 17 + (32 + 64) + 13 with an 11-bit ID.
    const auto standard = Fd(false, false, 8);
    zassert_equal(standard.nominal_bits, 126U);
    zassert_equal(standard.data_bits, 0U);
    zassert_equal(standard.time_ns, 252000U);

    // 36 + 96 + 13 with a 29-bit one.
    const auto extended = Fd(true, false, 8);
    zassert_equal(extended.nominal_bits, 145U);
    zassert_equal(extended.data_bits, 0U);

    // 17 + (37 + 512) + 13.
    zassert_equal(Fd(false, false, 64).nominal_bits, 579U);
}

ZTEST(can_monitor_bit_time, test_FdWithBrsSplitsTheFrameAtTheBitrateSwitch) {
    // Arbitration 17 and end 13 at 500 kbit/s, the data phase 32 + 64 at 2 Mbit/s.
    const auto standard = Fd(false, true, 8);
    zassert_equal(standard.nominal_bits, 30U);
    zassert_equal(standard.data_bits, 96U);
    zassert_equal(standard.time_ns, (30U * 2000U) + (96U * 500U));

    const auto extended = Fd(true, true, 8);
    zassert_equal(extended.nominal_bits, 49U);
    zassert_equal(extended.data_bits, 96U);
    zassert_equal(extended.time_ns, (49U * 2000U) + (96U * 500U));

    // 64 bytes: more than 16, so the longer CRC and its seven stuff bits: 37 + 512.
    const auto big = Fd(false, true, 64);
    zassert_equal(big.nominal_bits, 30U);
    zassert_equal(big.data_bits, 549U);
    zassert_equal(big.time_ns, (30U * 2000U) + (549U * 500U));

    const auto big_extended = Fd(true, true, 64);
    zassert_equal(big_extended.nominal_bits, 49U);
    zassert_equal(big_extended.data_bits, 549U);
}

ZTEST(can_monitor_bit_time, test_TheCrcLengthChangesAfter16DataBytes) {
    zassert_equal(Fd(false, true, 16).data_bits, 32U + 128U);
    zassert_equal(Fd(false, true, 20).data_bits, 37U + 160U);
    zassert_equal(Fd(false, true, 12).data_bits, 32U + 96U);
    zassert_equal(Fd(false, true, 0).data_bits, 32U);
}

ZTEST(can_monitor_bit_time, test_DataBitrateZeroMeansNominal) {
    const auto same = CanBitTime::Compute(false, true, true, false, 8, NOMINAL, 0);
    zassert_equal(same.nominal_bits, 30U);
    zassert_equal(same.data_bits, 96U);
    zassert_equal(same.time_ns, 126U * 2000U);
}

ZTEST(can_monitor_bit_time, test_TimesRoundToTheNearestNanosecond) {
    // One bit at 3 Mbit/s is 333.33 ns; 47 bits at 3 Mbit/s are 15666.67 ns.
    zassert_equal(CanBitTime::Compute(false, false, false, true, 0, 3000000, 0).time_ns, 15667U);

    // A bitrate of 0 has no time.
    zassert_equal(CanBitTime::Compute(false, false, false, false, 8, 0, 0).time_ns, 0U);
}

ZTEST(can_monitor_bit_time, test_TheFrameOverloadTakesTheLengthFromTheDlc) {
    const auto payload = MakePayload(12);
    const CanRecordFrame fd{ .id = 0x10, .dlc = 9, .fdf = true, .brs = true, .payload = payload };
    const auto from_frame = CanBitTime::Compute(fd, NOMINAL, DATA);
    const auto direct = CanBitTime::Compute(false, true, true, false, 12, NOMINAL, DATA);
    zassert_equal(from_frame.nominal_bits, direct.nominal_bits);
    zassert_equal(from_frame.data_bits, direct.data_bits);
    zassert_equal(from_frame.time_ns, direct.time_ns);

    // A classical frame with a DLC above 8 has 8 bytes.
    const auto eight = MakePayload(8);
    const CanRecordFrame classic{ .id = 0x10, .dlc = 12, .payload = eight };
    zassert_equal(CanBitTime::Compute(classic, NOMINAL, 0).nominal_bits, 111U);

    const CanRecordFrame remote{ .id = 0x10, .dlc = 8, .rtr = true };
    zassert_equal(CanBitTime::Compute(remote, NOMINAL, 0).nominal_bits, 47U);
}

ZTEST(can_monitor_bit_time, test_SummingNanosecondsDoesNotDrift) {
    // 1,000 classical frames at 1 Mbit/s: each is 111,000 ns exactly, whatever µs rounding would do.
    uint64_t total_ns = 0;
    for(size_t i = 0; i < 1000; ++i)
        total_ns += CanBitTime::Compute(false, false, false, false, 8, 1000000, 0).time_ns;
    zassert_equal(total_ns / 1000, 111000U);
}
