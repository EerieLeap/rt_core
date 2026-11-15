#include <zephyr/ztest.h>

#include <array>
#include <climits>
#include <cstdint>
#include <stdexcept>

#include "domain/can_monitor_domain/transmit/can_tx_limiter.h"
#include "domain/can_monitor_domain/utilities/can_record.h"
#include "subsys/canbus/can_frame.h"

using namespace eerie_leap::domain::can_monitor_domain::transmit;
using eerie_leap::domain::can_monitor_domain::utilities::CanRecordFrame;
using eerie_leap::subsys::canbus::CanFrame;

ZTEST_SUITE(can_tx_limiter, NULL, NULL, NULL, NULL, NULL);

namespace {

constexpr int64_t US_PER_SECOND = 1'000'000;
constexpr int64_t NS_PER_US = 1000;

// 8 data bytes, 29-bit ID, classical: 67 + 64 = 131 bits.
constexpr uint64_t CLASSICAL_8_EXT_500K_NS = uint64_t{ 131 } * 2000;
// No data, 11-bit ID, classical: 47 bits.
constexpr uint64_t CLASSICAL_0_STD_500K_NS = uint64_t{ 47 } * 2000;

unsigned long long Ull(uint64_t value) {
    return value;
}

CanFrame Classical(bool ide, size_t size) {
    CanFrame frame;
    frame.is_extended = ide;
    frame.data.Resize(size);

    return frame;
}

CanFrame Fd(bool ide, bool brs, size_t size) {
    CanFrame frame = Classical(ide, size);
    frame.is_can_fd = true;
    frame.is_bitrate_switch = brs;

    return frame;
}

// Takes frames whenever the bucket allows and waits exactly as long as WaitUs() says otherwise, to now_us.
// Returns the bus time taken in ns.
uint64_t DriveToTime(CanTxLimiter& limiter, int64_t now_us, int64_t end_us, uint64_t cost_ns, int64_t& stopped_at) {
    uint64_t taken_ns = 0;

    while(true) {
        limiter.Update(now_us);

        if(limiter.TryTake(cost_ns)) {
            taken_ns += cost_ns;
            continue;
        }

        const int64_t wait_us = limiter.WaitUs(cost_ns);
        zassert_true(wait_us > 0, "A refused frame must have a wait");

        if(now_us + wait_us > end_us)
            break;

        now_us += wait_us;
    }

    stopped_at = now_us;

    return taken_ns;
}

} // namespace

ZTEST(can_tx_limiter, test_ABucketIsAsDeepAsBurstFramesOfTheLargestFrame) {
    CanTxLimiter classical(500000, 0, false, 50, 8, 0);
    zassert_equal(classical.CapacityNs(), 8 * CLASSICAL_8_EXT_500K_NS);
    zassert_equal(classical.CreditNs(), classical.CapacityNs(), "Starts full");

    // 64 bytes, 29-bit, BRS: arbitration and end (36 + 13 bits) at 500 kbit/s, the data (37 + 512) at 2 Mbit/s.
    CanTxLimiter fd(500000, 2000000, true, 50, 8, 0);
    constexpr uint64_t FD_LARGEST_NS = (49 * 2000) + (549 * 500);
    zassert_equal(fd.CapacityNs(), 8 * FD_LARGEST_NS);
    zassert_equal(fd.CostNs(Fd(true, true, 64)), FD_LARGEST_NS);

    // No data bitrate: the data phase runs at the nominal one.
    CanTxLimiter fd_one_rate(500000, 0, true, 50, 4, 0);
    zassert_equal(fd_one_rate.CapacityNs(), uint64_t{ 4 } * 598 * 2000);

    // A classical channel ignores the data bitrate.
    CanTxLimiter classical_with_data(500000, 2000000, false, 50, 8, 0);
    zassert_equal(classical_with_data.CapacityNs(), 8 * CLASSICAL_8_EXT_500K_NS);
}

ZTEST(can_tx_limiter, test_TheCostOfAFrameIsItsBitTime) {
    CanTxLimiter limiter(500000, 2000000, true, 50, 8, 0);

    zassert_equal(limiter.CostNs(Classical(false, 0)), CLASSICAL_0_STD_500K_NS);
    zassert_equal(limiter.CostNs(Classical(true, 8)), CLASSICAL_8_EXT_500K_NS);

    CanFrame remote = Classical(true, 0);
    remote.is_remote_request = true;
    zassert_equal(limiter.CostNs(remote), (uint64_t{ 67 } * 2000), "A remote frame has no data field");

    // The same through the record the encoders take: the data field's length comes from the DLC.
    const CanRecordFrame record = { .id = 0x123, .dlc = 8, .ide = true };
    zassert_equal(limiter.CostNs(record), CLASSICAL_8_EXT_500K_NS);

    const CanRecordFrame fd_record = { .id = 0x123, .dlc = 15, .ide = true, .fdf = true, .brs = true };
    zassert_equal(limiter.CostNs(fd_record), (49 * 2000U) + (549 * 500U));
}

ZTEST(can_tx_limiter, test_AnIdleBucketPassesABurstThenRefuses) {
    CanTxLimiter limiter(500000, 0, false, 50, 8, 1000);
    limiter.Update(1000);

    for(int i = 0; i < 8; ++i)
        zassert_true(limiter.TryTake(CLASSICAL_8_EXT_500K_NS), "Frame %d of the burst", i);

    zassert_false(limiter.TryTake(CLASSICAL_8_EXT_500K_NS), "The ninth frame is over the burst");
    zassert_equal(limiter.CreditNs(), 0U);

    // Smaller frames: the bucket holds 2,096,000 ns, 22 frames of 94,000 ns and 28,000 ns over.
    limiter.Reset(1000);
    int passed = 0;
    while(limiter.TryTake(CLASSICAL_0_STD_500K_NS))
        ++passed;

    zassert_equal(passed, 22);
    zassert_equal(limiter.CreditNs(), 2'096'000U - (22 * 94'000U));
}

ZTEST(can_tx_limiter, test_ARefusedFrameCostsNothing) {
    CanTxLimiter limiter(500000, 0, false, 50, 1, 0);

    zassert_true(limiter.TryTake(CLASSICAL_8_EXT_500K_NS - 1));
    zassert_equal(limiter.CreditNs(), 1U);
    zassert_false(limiter.TryTake(2));
    zassert_equal(limiter.CreditNs(), 1U);
}

ZTEST(can_tx_limiter, test_TheBucketRefillsAtTheShareOfTheElapsedTime) {
    CanTxLimiter limiter(500000, 0, false, 50, 8, 0);

    for(int i = 0; i < 8; ++i)
        zassert_true(limiter.TryTake(CLASSICAL_8_EXT_500K_NS));

    // 50 %: one µs of time earns 500 ns of bus time.
    limiter.Update(100);
    zassert_equal(limiter.CreditNs(), 50'000U);

    limiter.Update(1100);
    zassert_equal(limiter.CreditNs(), 550'000U);

    // A frame of 262,000 ns needs 524 µs from empty.
    limiter.Reset(0);
    for(int i = 0; i < 8; ++i)
        zassert_true(limiter.TryTake(CLASSICAL_8_EXT_500K_NS));

    limiter.Update(523);
    zassert_false(limiter.TryTake(CLASSICAL_8_EXT_500K_NS));
    limiter.Update(524);
    zassert_true(limiter.TryTake(CLASSICAL_8_EXT_500K_NS));
}

ZTEST(can_tx_limiter, test_TheBucketNeverHoldsMoreThanItsDepth) {
    CanTxLimiter limiter(500000, 0, false, 100, 8, 0);

    zassert_true(limiter.TryTake(CLASSICAL_8_EXT_500K_NS));
    limiter.Update(3600 * US_PER_SECOND);
    zassert_equal(limiter.CreditNs(), limiter.CapacityNs());

    // A far-off time must not overflow the arithmetic.
    limiter.Update(INT64_MAX / 2);
    zassert_equal(limiter.CreditNs(), limiter.CapacityNs());
    zassert_true(limiter.TryTake(CLASSICAL_8_EXT_500K_NS));
}

ZTEST(can_tx_limiter, test_WaitUsIsExact) {
    // 100 %: the wait in µs is the cost in µs.
    {
        CanTxLimiter limiter(500000, 0, false, 100, 1, 0);
        zassert_equal(limiter.WaitUs(100'000), 0, "A full bucket makes no frame wait");

        zassert_true(limiter.TryTake(CLASSICAL_8_EXT_500K_NS));
        zassert_equal(limiter.WaitUs(100'000), 100);
        zassert_equal(limiter.WaitUs(1'000), 1);
        zassert_equal(limiter.WaitUs(1), 1, "Rounded up to the next µs");
        zassert_equal(limiter.WaitUs(0), 0);

        limiter.Update(100);
        zassert_equal(limiter.WaitUs(100'000), 0);
        zassert_true(limiter.TryTake(100'000));
    }

    // 25 %: 262,000 ns of bus time takes 1,048 µs of time.
    {
        CanTxLimiter limiter(500000, 0, false, 25, 8, 0);
        for(int i = 0; i < 8; ++i)
            zassert_true(limiter.TryTake(CLASSICAL_8_EXT_500K_NS));

        zassert_equal(limiter.WaitUs(CLASSICAL_8_EXT_500K_NS), 1048);
        limiter.Update(1000);
        zassert_equal(limiter.WaitUs(CLASSICAL_8_EXT_500K_NS), 48);
        limiter.Update(1047);
        zassert_equal(limiter.WaitUs(CLASSICAL_8_EXT_500K_NS), 1);
        zassert_false(limiter.TryTake(CLASSICAL_8_EXT_500K_NS));
        limiter.Update(1048);
        zassert_equal(limiter.WaitUs(CLASSICAL_8_EXT_500K_NS), 0);
        zassert_true(limiter.TryTake(CLASSICAL_8_EXT_500K_NS));
    }

    // 30 %: 94,000 ns takes 313.33 µs, so 314.
    {
        CanTxLimiter limiter(500000, 0, false, 30, 1, 0);
        zassert_true(limiter.TryTake(CLASSICAL_8_EXT_500K_NS - 1));
        zassert_true(limiter.TryTake(1));
        zassert_equal(limiter.WaitUs(94'000), 314);
        limiter.Update(313);
        zassert_false(limiter.TryTake(94'000));
        zassert_equal(limiter.WaitUs(94'000), 1);
        limiter.Update(314);
        zassert_true(limiter.TryTake(94'000));
    }

    // 1 %: the longest waits.
    {
        CanTxLimiter limiter(500000, 0, false, 1, 1, 0);
        zassert_true(limiter.TryTake(CLASSICAL_8_EXT_500K_NS));
        zassert_equal(limiter.WaitUs(94'000), 9400);
    }
}

ZTEST(can_tx_limiter, test_AFrameBiggerThanTheBucketTakesTheWholeBucket) {
    // A burst of 1 holds one largest frame; an FD frame without BRS at 500 kbit/s all through is longer.
    CanTxLimiter limiter(500000, 2000000, true, 50, 1, 0);
    const CanFrame slow = Fd(true, false, 64);
    const uint64_t cost = limiter.CostNs(slow);

    zassert_true(cost > limiter.CapacityNs());
    zassert_true(limiter.TryTake(cost), "It must pass an idle bucket, or it never could");
    zassert_equal(limiter.CreditNs(), 0U);
    zassert_false(limiter.TryTake(cost));

    // And it waits for the bucket to fill, not for more than that.
    zassert_equal(limiter.WaitUs(cost), static_cast<int64_t>(limiter.CapacityNs() / 500));
    limiter.Update(limiter.WaitUs(cost));
    zassert_true(limiter.TryTake(cost));
}

ZTEST(can_tx_limiter, test_TheLongRunRateIsTheShareWithinATenthOfAPercent) {
    for(const uint8_t share : std::array<uint8_t, 4>{ 1, 25, 50, 100 }) {
        CanTxLimiter limiter(500000, 0, false, share, 8, 0);

        int64_t end = 0;
        const uint64_t taken_ns = DriveToTime(limiter, 0, 60 * US_PER_SECOND, CLASSICAL_8_EXT_500K_NS, end);

        // Supply: the full bucket at the start and the share of the time until the last frame went.
        const uint64_t supply_ns = limiter.CapacityNs() + (static_cast<uint64_t>(end) * NS_PER_US * share / 100);
        zassert_true(taken_ns <= supply_ns, "Took %llu of %llu ns at %u %%", Ull(taken_ns), Ull(supply_ns), share);
        zassert_true(supply_ns - taken_ns < CLASSICAL_8_EXT_500K_NS, "Left %llu ns unused", Ull(supply_ns - taken_ns));

        // The minute's bus time, less the burst at the start, is the share of it: within 0.1 %.
        const uint64_t expected_ns = 60ULL * 1'000'000'000ULL * share / 100;
        const uint64_t after_burst_ns = taken_ns - limiter.CapacityNs();
        zassert_true(
            after_burst_ns * 1000 > expected_ns * 999 && after_burst_ns * 1000 < expected_ns * 1001,
            "Took %llu ns of an expected %llu at %u %%", Ull(after_burst_ns), Ull(expected_ns), share
        );
    }
}

ZTEST(can_tx_limiter, test_OddBitratesDoNotDrift) {
    // 83,333 bit/s has no whole ns per bit, so every cost is rounded; the credit must still add up exactly.
    CanTxLimiter limiter(83333, 0, false, 33, 3, 0);
    const uint64_t cost_ns = limiter.CostNs(Classical(true, 8));

    int64_t end = 0;
    const uint64_t taken_ns = DriveToTime(limiter, 0, 600 * US_PER_SECOND, cost_ns, end);

    const uint64_t supply_ns = limiter.CapacityNs() + (static_cast<uint64_t>(end) * NS_PER_US * 33 / 100);
    zassert_true(taken_ns <= supply_ns);
    zassert_true(
        supply_ns - taken_ns < cost_ns, "Ten minutes left %llu ns of a %llu ns frame", Ull(supply_ns - taken_ns),
        Ull(cost_ns)
    );
}

ZTEST(can_tx_limiter, test_PollingAtAnyRhythmGivesTheSameBusTime) {
    // The same ten seconds polled every 997 µs and every 5 ms (each poll earns less than the bucket holds):
    // the credit depends on the time, not on the polls.
    for(const int64_t step : { int64_t{ 997 }, int64_t{ 5000 } }) {
        CanTxLimiter limiter(83333, 0, false, 33, 3, 5000);
        const uint64_t cost_ns = limiter.CostNs(Classical(false, 3));
        uint64_t taken_ns = 0;
        int64_t now = 5000;

        // A full bucket earns nothing, so the burst goes first.
        while(limiter.TryTake(cost_ns))
            taken_ns += cost_ns;

        while(now - 5000 < 10 * US_PER_SECOND) {
            now += step;
            limiter.Update(now);
            while(limiter.TryTake(cost_ns))
                taken_ns += cost_ns;
        }

        const uint64_t supply_ns = limiter.CapacityNs() + (static_cast<uint64_t>(now - 5000) * NS_PER_US * 33 / 100);
        zassert_true(taken_ns <= supply_ns);
        zassert_true(
            supply_ns - taken_ns < cost_ns, "Step %lld left %llu ns", static_cast<long long>(step),
            Ull(supply_ns - taken_ns)
        );
    }
}

ZTEST(can_tx_limiter, test_TimeGoingBackwardsAddsNothing) {
    CanTxLimiter limiter(500000, 0, false, 50, 8, 10'000);

    for(int i = 0; i < 8; ++i)
        zassert_true(limiter.TryTake(CLASSICAL_8_EXT_500K_NS));

    limiter.Update(10'100);
    zassert_equal(limiter.CreditNs(), 50'000U);

    limiter.Update(9'000);
    zassert_equal(limiter.CreditNs(), 50'000U, "A time before the last update adds nothing");

    limiter.Update(10'100);
    zassert_equal(limiter.CreditNs(), 50'000U, "Nor does the same time again");

    // The clock returning to where it was earns nothing for the way back; only the time past the last update counts.
    limiter.Update(10'110);
    zassert_equal(limiter.CreditNs(), 55'000U);
}

ZTEST(can_tx_limiter, test_ResetFillsTheBucketAtTheNewTime) {
    CanTxLimiter limiter(500000, 0, false, 50, 8, 0);

    for(int i = 0; i < 8; ++i)
        zassert_true(limiter.TryTake(CLASSICAL_8_EXT_500K_NS));

    limiter.Reset(1'000'000);
    zassert_equal(limiter.CreditNs(), limiter.CapacityNs());

    // Time counts from the reset.
    zassert_true(limiter.TryTake(limiter.CapacityNs()));
    limiter.Update(1'000'100);
    zassert_equal(limiter.CreditNs(), 50'000U);
}

ZTEST(can_tx_limiter, test_ResetWithNewParametersReplacesTheLimiter) {
    CanTxLimiter limiter(500000, 0, false, 50, 8, 0);

    limiter.Reset(1000000, 4000000, true, 20, 2, 500);
    zassert_equal(limiter.CostNs(Classical(false, 0)), uint64_t{ 47 } * 1000);
    zassert_equal(limiter.CapacityNs(), uint64_t{ 2 } * ((uint64_t{ 49 } * 1000) + (uint64_t{ 549 } * 250)));

    // Rejected parameters leave it as it was.
    bool thrown = false;
    try {
        limiter.Reset(0, 0, false, 50, 8, 0);
    } catch(const std::invalid_argument&) {
        thrown = true;
    }
    zassert_true(thrown);
    zassert_equal(limiter.CostNs(Classical(false, 0)), uint64_t{ 47 } * 1000);
}

ZTEST(can_tx_limiter, test_ADefaultLimiterAndInvalidParametersLetNothingThrough) {
    CanTxLimiter never;
    never.Update(1'000'000);
    zassert_false(never.TryTake(0));
    zassert_false(never.TryTake(1000));
    zassert_equal(never.WaitUs(1000), INT64_MAX);
    zassert_equal(never.CapacityNs(), 0U);

    int rejected = 0;
    const auto rejects = [&](uint32_t nominal, uint8_t share, uint32_t burst) {
        try {
            CanTxLimiter limiter(nominal, 0, false, share, burst, 0);
        } catch(const std::invalid_argument&) {
            ++rejected;
        }
    };

    rejects(0, 50, 8);
    rejects(500000, 0, 8);
    rejects(500000, 101, 8);
    rejects(500000, 50, 0);
    zassert_equal(rejected, 4);
}

ZTEST(can_tx_limiter, test_TheDefaultsComeFromKconfig) {
    CanTxLimiter limiter(500000, 0, false, 0);

    zassert_equal(CanTxLimiter::DEFAULT_SHARE_PERCENT, CONFIG_EERIE_LEAP_CAN_MONITOR_TX_SHARE_PERCENT);
    zassert_equal(CanTxLimiter::DEFAULT_BURST_FRAMES, CONFIG_EERIE_LEAP_CAN_MONITOR_TX_BURST_FRAMES);
    zassert_equal(limiter.CapacityNs(), CONFIG_EERIE_LEAP_CAN_MONITOR_TX_BURST_FRAMES * CLASSICAL_8_EXT_500K_NS);

    // After a burst, a frame waits for the configured share of the time.
    while(limiter.TryTake(CLASSICAL_8_EXT_500K_NS)) {
    }
    zassert_equal(
        limiter.WaitUs(CLASSICAL_8_EXT_500K_NS),
        static_cast<int64_t>(
            ((CLASSICAL_8_EXT_500K_NS - limiter.CreditNs()) * 100
                + (uint64_t{ CONFIG_EERIE_LEAP_CAN_MONITOR_TX_SHARE_PERCENT } * 1000 - 1))
            / (uint64_t{ CONFIG_EERIE_LEAP_CAN_MONITOR_TX_SHARE_PERCENT } * 1000)
        )
    );
}
