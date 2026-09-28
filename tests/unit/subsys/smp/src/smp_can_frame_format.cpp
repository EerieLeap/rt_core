#include <zephyr/ztest.h>

#include "subsys/smp/can/smp_can_frame_format.h"

using eerie_leap::subsys::smp::can::SmpCanFrameFormat;

ZTEST_SUITE(smp_can_frame_format, NULL, NULL, NULL, NULL, NULL);

ZTEST(smp_can_frame_format, test_classic_frames_are_8_bytes) {
    constexpr SmpCanFrameFormat classic;

    zassert_false(classic.IsCanFd());
    zassert_equal(classic.GetFrameSize(), 8);
    zassert_equal(classic.GetDataSize(), 7);
    zassert_equal(classic.GetPaddedSize(5), 5);
    zassert_equal(classic.GetFrameCount(15), 3);
}

ZTEST(smp_can_frame_format, test_can_fd_frames_are_64_bytes) {
    constexpr SmpCanFrameFormat can_fd(true);

    zassert_true(can_fd.IsCanFd());
    zassert_equal(can_fd.GetFrameSize(), 64);
    zassert_equal(can_fd.GetDataSize(), 63);
    zassert_equal(can_fd.GetFrameCount(126), 2);
    zassert_equal(can_fd.GetFrameCount(127), 3);
}

ZTEST(smp_can_frame_format, test_can_fd_lengths_round_up_to_a_valid_dlc) {
    constexpr SmpCanFrameFormat can_fd(true);

    zassert_equal(can_fd.GetPaddedSize(8), 8);
    zassert_equal(can_fd.GetPaddedSize(9), 12);
    zassert_equal(can_fd.GetPaddedSize(13), 16);
    zassert_equal(can_fd.GetPaddedSize(24), 24);
    zassert_equal(can_fd.GetPaddedSize(25), 32);
    zassert_equal(can_fd.GetPaddedSize(33), 48);
    zassert_equal(can_fd.GetPaddedSize(49), 64);
    zassert_equal(can_fd.GetPaddedSize(64), 64);
}

ZTEST(smp_can_frame_format, test_frame_time_follows_the_bitrates) {
    constexpr SmpCanFrameFormat classic;
    constexpr SmpCanFrameFormat can_fd(true);

    zassert_equal(classic.GetFrameTimeNs(500000), 320000);
    zassert_equal(classic.GetFrameTimeNs(500000, 2000000), 320000, "Classic frames ignore the data bitrate");
    zassert_equal(can_fd.GetFrameTimeNs(500000, 2000000), 470000);
    zassert_equal(can_fd.GetFrameTimeNs(500000), 1520000, "Without a data bitrate the data phase runs at 500 kbit/s");
    zassert_equal(can_fd.GetFrameTimeNs(0, 2000000), 0);
}
