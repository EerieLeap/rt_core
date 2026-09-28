#include <stdexcept>

#include <zephyr/ztest.h>

#include "subsys/smp/can/smp_can_frame_format.h"
#include "subsys/smp/can/smp_can_pacer.h"

using eerie_leap::subsys::smp::can::SmpCanFrameFormat;
using eerie_leap::subsys::smp::can::SmpCanPacer;

ZTEST_SUITE(smp_can_pacer, NULL, NULL, NULL, NULL, NULL);

namespace {

constexpr uint32_t BITRATE = 500000;
constexpr uint8_t SHARE = 25;
constexpr uint32_t BURST = 8;

// 160 bit times at 500 kbit/s.
constexpr uint32_t FRAME_TIME_NS = SmpCanFrameFormat().GetFrameTimeNs(BITRATE);

// 25 % of the bus time / 320 us per frame.
constexpr uint32_t FRAMES_PER_SECOND = 781;

SmpCanPacer MakePacer() {
    return SmpCanPacer(FRAME_TIME_NS, SHARE, BURST);
}

uint32_t SendAvailable(SmpCanPacer& pacer) {
    uint32_t sent = 0;
    while(pacer.CanSend()) {
        pacer.OnSent();
        sent++;
    }

    return sent;
}

} // namespace

ZTEST(smp_can_pacer, test_rate_is_the_share_of_the_bus_time) {
    zassert_equal(FRAME_TIME_NS, 320000);
    zassert_equal(MakePacer().GetFramesPerSecond(), FRAMES_PER_SECOND);
}

ZTEST(smp_can_pacer, test_can_fd_rate_follows_both_bitrates) {
    // 60 bit times at 500 kbit/s + 700 at 2 Mbit/s = 470 us.
    SmpCanPacer pacer(SmpCanFrameFormat(true).GetFrameTimeNs(BITRATE, 2000000), SHARE, BURST);

    zassert_equal(pacer.GetFramesPerSecond(), 531);
}

ZTEST(smp_can_pacer, test_burst_is_available_after_reset) {
    SmpCanPacer pacer = MakePacer();
    pacer.Reset(0);

    zassert_equal(SendAvailable(pacer), BURST);
    zassert_true(pacer.GetWaitUs() > 0);
}

ZTEST(smp_can_pacer, test_wait_matches_one_frame_time) {
    SmpCanPacer pacer = MakePacer();
    pacer.Reset(0);
    SendAvailable(pacer);

    // A 320 us frame at a 25 % share.
    zassert_equal(pacer.GetWaitUs(), 1280);

    pacer.Update(1279);
    zassert_false(pacer.CanSend());

    pacer.Update(1280);
    zassert_true(pacer.CanSend());
}

ZTEST(smp_can_pacer, test_wait_for_several_frames) {
    SmpCanPacer pacer = MakePacer();
    pacer.Reset(0);
    SendAvailable(pacer);

    zassert_equal(pacer.GetWaitUs(4), 4 * 1280);
    zassert_equal(pacer.GetWaitUs(100), BURST * 1280, "Never waits for more than the bucket holds");

    pacer.Update(4 * 1280);
    zassert_equal(pacer.GetWaitUs(4), 0);
    zassert_equal(SendAvailable(pacer), 4);
}

ZTEST(smp_can_pacer, test_long_run_rate_stays_within_the_share) {
    SmpCanPacer pacer = MakePacer();
    pacer.Reset(0);

    uint32_t sent = 0;
    for(int64_t now_us = 0; now_us <= 1'000'000; now_us += 100) {
        pacer.Update(now_us);
        sent += SendAvailable(pacer);
    }

    zassert_true(sent <= FRAMES_PER_SECOND + BURST + 1, "Sent %u frames in 1 s", sent);
    zassert_true(sent >= FRAMES_PER_SECOND, "Sent %u frames in 1 s", sent);
}

ZTEST(smp_can_pacer, test_coarse_ticks_cannot_exceed_the_burst) {
    SmpCanPacer pacer = MakePacer();
    pacer.Reset(0);
    SendAvailable(pacer);

    pacer.Update(10'000'000);

    zassert_equal(SendAvailable(pacer), BURST, "Idle time only refills the bucket");
}

ZTEST(smp_can_pacer, test_time_going_backwards_adds_no_credit) {
    SmpCanPacer pacer = MakePacer();
    pacer.Reset(5000);
    SendAvailable(pacer);

    pacer.Update(1000);

    zassert_false(pacer.CanSend());
}

ZTEST(smp_can_pacer, test_default_pacer_never_sends) {
    SmpCanPacer pacer;
    pacer.Reset(0);

    zassert_false(pacer.CanSend());
}

ZTEST(smp_can_pacer, test_invalid_parameters_are_rejected) {
    auto rejects = [](uint32_t frame_time_ns, uint8_t share, uint32_t burst) {
        try {
            SmpCanPacer pacer(frame_time_ns, share, burst);
            (void)pacer;
        } catch(const std::invalid_argument&) {
            return true;
        }

        return false;
    };

    zassert_true(rejects(0, SHARE, BURST));
    zassert_true(rejects(FRAME_TIME_NS, 0, BURST));
    zassert_true(rejects(FRAME_TIME_NS, 101, BURST));
    zassert_true(rejects(FRAME_TIME_NS, SHARE, 0));
    zassert_false(rejects(FRAME_TIME_NS, 100, 1));
}
