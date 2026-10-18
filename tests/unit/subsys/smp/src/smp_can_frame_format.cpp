#include <zephyr/ztest.h>

#include "subsys/smp/can/smp_can_frame_format.h"

using eerie_leap::subsys::smp::can::SmpCanFrameFormat;

ZTEST_SUITE(smp_can_frame_format, NULL, NULL, NULL, NULL, NULL);

ZTEST(smp_can_frame_format, test_classic_frames_are_8_bytes) {
    constexpr SmpCanFrameFormat CLASSIC;

    zassert_false(CLASSIC.IsCanFd());
    zassert_equal(CLASSIC.GetFrameSize(), 8);
    zassert_equal(CLASSIC.GetDataSize(), 7);
    zassert_equal(CLASSIC.GetPaddedSize(5), 5);
    zassert_equal(CLASSIC.GetFrameCount(15), 3);
}

ZTEST(smp_can_frame_format, test_can_fd_frames_are_64_bytes) {
    constexpr SmpCanFrameFormat CAN_FD(true);

    zassert_true(CAN_FD.IsCanFd());
    zassert_equal(CAN_FD.GetFrameSize(), 64);
    zassert_equal(CAN_FD.GetDataSize(), 63);
    zassert_equal(CAN_FD.GetFrameCount(126), 2);
    zassert_equal(CAN_FD.GetFrameCount(127), 3);
}

ZTEST(smp_can_frame_format, test_can_fd_lengths_round_up_to_a_valid_dlc) {
    constexpr SmpCanFrameFormat CAN_FD(true);

    zassert_equal(CAN_FD.GetPaddedSize(8), 8);
    zassert_equal(CAN_FD.GetPaddedSize(9), 12);
    zassert_equal(CAN_FD.GetPaddedSize(13), 16);
    zassert_equal(CAN_FD.GetPaddedSize(24), 24);
    zassert_equal(CAN_FD.GetPaddedSize(25), 32);
    zassert_equal(CAN_FD.GetPaddedSize(33), 48);
    zassert_equal(CAN_FD.GetPaddedSize(49), 64);
    zassert_equal(CAN_FD.GetPaddedSize(64), 64);
}

ZTEST(smp_can_frame_format, test_frame_time_follows_the_bitrates) {
    constexpr SmpCanFrameFormat CLASSIC;
    constexpr SmpCanFrameFormat CAN_FD(true);

    zassert_equal(CLASSIC.GetFrameTimeNs(500000), 320000);
    zassert_equal(CLASSIC.GetFrameTimeNs(500000, 2000000), 320000, "Classic frames ignore the data bitrate");
    zassert_equal(CAN_FD.GetFrameTimeNs(500000, 2000000), 470000);
    zassert_equal(CAN_FD.GetFrameTimeNs(500000), 1520000, "Without a data bitrate the data phase runs at 500 kbit/s");
    zassert_equal(CAN_FD.GetFrameTimeNs(0, 2000000), 0);
}
