#include <algorithm>

#include <zephyr/ztest.h>

#include "smp_test_support.h"

using namespace smp_test;

ZTEST_SUITE(smp_can_framer, NULL, NULL, NULL, NULL, NULL);

ZTEST(smp_can_framer, test_first_frame_is_marked_start) {
    auto packet = MakePacket(20);
    auto frames = MakeFrames(packet);

    zassert_equal(frames.size(), SmpCanFramer::GetFrameCount(packet.size()));
    zassert_equal(frames.size(), 4);
    zassert_equal(frames[0][0], SmpCanFrameFormat::START_FLAG);
    zassert_equal(frames[1][0], 1);
    zassert_equal(frames[3][0], 3);
}

ZTEST(smp_can_framer, test_only_the_last_frame_is_short) {
    auto packet = MakePacket(20); // 28 bytes: 7 + 7 + 7 + 7
    auto odd_packet = MakePacket(22); // 30 bytes: 7 + 7 + 7 + 7 + 2

    for(const auto& frame : MakeFrames(packet))
        zassert_equal(frame.size(), SmpCanFrameFormat::CLASSIC_FRAME_SIZE);

    auto odd_frames = MakeFrames(odd_packet);
    zassert_equal(odd_frames.size(), 5);
    zassert_equal(odd_frames.back().size(), 3);
}

ZTEST(smp_can_framer, test_frames_carry_the_packet_in_order) {
    auto packet = MakePacket(40);
    std::vector<uint8_t> joined;

    for(const auto& frame : MakeFrames(packet))
        joined.insert(joined.end(), frame.begin() + 1, frame.end());

    zassert_true(joined == packet);
}

ZTEST(smp_can_framer, test_sequence_wraps_after_127) {
    auto packet = MakePacket(1000);
    auto frames = MakeFrames(packet);

    zassert_true(frames.size() > 129);
    zassert_equal(frames[127][0], 127);
    zassert_equal(frames[128][0], 0, "Frame 128 wraps to sequence 0 without the start flag");
    zassert_equal(frames[129][0], 1);
}

ZTEST(smp_can_framer, test_empty_packet_has_no_frames) {
    SmpCanFramer framer(std::span<const uint8_t>{});
    std::array<uint8_t, SmpCanFrameFormat::MAX_FRAME_SIZE> frame{};

    zassert_true(framer.IsDone());
    zassert_equal(framer.Encode(frame), 0);
    zassert_equal(SmpCanFramer::GetFrameCount(0), 0);
}

ZTEST(smp_can_framer, test_encode_does_not_advance) {
    auto packet = MakePacket(10);
    SmpCanFramer framer(packet);
    std::array<uint8_t, SmpCanFrameFormat::MAX_FRAME_SIZE> first{};
    std::array<uint8_t, SmpCanFrameFormat::MAX_FRAME_SIZE> again{};

    framer.Encode(first);
    framer.Encode(again);

    zassert_true(first == again, "A frame that failed to send must be encoded again unchanged");
}

ZTEST(smp_can_framer, test_can_fd_frames_carry_63_bytes) {
    constexpr SmpCanFrameFormat can_fd(true);
    auto packet = MakePacket(118); // 126 bytes: 63 + 63
    auto frames = MakeFrames(packet, can_fd);

    zassert_equal(frames.size(), 2);
    zassert_equal(frames[0].size(), SmpCanFrameFormat::CAN_FD_FRAME_SIZE);
    zassert_equal(frames[1].size(), SmpCanFrameFormat::CAN_FD_FRAME_SIZE);
    zassert_equal(frames[1][0], 1);
    zassert_equal(SmpCanFramer::GetFrameCount(1008, can_fd), 16);
}

ZTEST(smp_can_framer, test_short_can_fd_frame_is_padded_to_a_valid_length) {
    constexpr SmpCanFrameFormat can_fd(true);
    auto packet = MakePacket(68); // 76 bytes: 63 + 13, sent as a 16-byte frame
    auto frames = MakeFrames(packet, can_fd);

    zassert_equal(frames.size(), 2);
    zassert_equal(frames[1].size(), 16);
    zassert_true(std::equal(packet.begin() + 63, packet.end(), frames[1].begin() + 1));
    zassert_true(std::all_of(frames[1].begin() + 14, frames[1].end(), [](uint8_t byte) { return byte == 0; }),
        "Padding is zeroed");
}

ZTEST(smp_can_framer, test_can_fd_frames_up_to_8_bytes_are_not_padded) {
    constexpr SmpCanFrameFormat can_fd(true);
    auto frames = MakeFrames(MakePacket(60), can_fd); // 68 bytes: 63 + 5

    zassert_equal(frames.size(), 2);
    zassert_equal(frames[1].size(), 6);

    auto header_only = MakeFrames(MakePacket(0), can_fd);
    zassert_equal(header_only[0].size(), 12, "9 bytes go out as a 12-byte frame");
}
