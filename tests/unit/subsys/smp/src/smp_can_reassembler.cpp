#include <algorithm>
#include <array>
#include <vector>

#include <zephyr/ztest.h>

#include "subsys/smp/can/smp_can_reassembler.h"

#include "smp_test_support.h"

using namespace smp_test;
using eerie_leap::subsys::smp::can::SmpCanReassembler;

using Result = SmpCanReassembler::Result;

ZTEST_SUITE(smp_can_reassembler, NULL, NULL, NULL, NULL, NULL);

namespace {

constexpr int64_t TIMEOUT_MS = SmpCanReassembler::DEFAULT_TIMEOUT_MS;

struct Receiver {
    std::array<uint8_t, 1024> buffer{};
    SmpCanReassembler reassembler;

    explicit Receiver(SmpCanFrameFormat format = {}) { reassembler.Attach(buffer, format); }

    Result Feed(const std::vector<Frame>& frames, int64_t now_ms = 0) {
        Result result = Result::IGNORED;
        for(const auto& frame : frames)
            result = reassembler.Accept(frame, now_ms);

        return result;
    }

    bool Holds(const std::vector<uint8_t>& packet) const {
        return reassembler.GetLength() == packet.size()
            && std::equal(packet.begin(), packet.end(), buffer.begin());
    }
};

} // namespace

ZTEST(smp_can_reassembler, test_single_frame_packet_completes) {
    Receiver receiver;
    auto packet = MakePacket(0);

    zassert_equal(receiver.Feed(MakeFrames(packet)), Result::COMPLETE);
    zassert_true(receiver.Holds(packet));
    zassert_false(receiver.reassembler.IsActive());
}

ZTEST(smp_can_reassembler, test_packets_of_every_frame_boundary_round_trip) {
    for(size_t body_size = 0; body_size <= 30; body_size++) {
        Receiver receiver;
        auto packet = MakePacket(body_size);

        zassert_equal(receiver.Feed(MakeFrames(packet)), Result::COMPLETE, "Body size %zu", body_size);
        zassert_true(receiver.Holds(packet), "Body size %zu", body_size);
    }
}

ZTEST(smp_can_reassembler, test_sequence_wrap_is_accepted) {
    Receiver receiver;
    auto packet = MakePacket(1000);

    zassert_equal(receiver.Feed(MakeFrames(packet)), Result::COMPLETE);
    zassert_true(receiver.Holds(packet));
}

ZTEST(smp_can_reassembler, test_intermediate_frames_are_in_progress) {
    Receiver receiver;
    auto frames = MakeFrames(MakePacket(20));

    for(size_t i = 0; i + 1 < frames.size(); i++)
        zassert_equal(receiver.reassembler.Accept(frames[i], 0), Result::IN_PROGRESS);

    zassert_true(receiver.reassembler.IsActive());
}

ZTEST(smp_can_reassembler, test_sequence_gap_drops_the_packet) {
    Receiver receiver;
    auto frames = MakeFrames(MakePacket(30));
    frames.erase(frames.begin() + 2);

    zassert_equal(receiver.reassembler.Accept(frames[0], 0), Result::IN_PROGRESS);
    zassert_equal(receiver.reassembler.Accept(frames[1], 0), Result::IN_PROGRESS);
    zassert_equal(receiver.reassembler.Accept(frames[2], 0), Result::DROPPED);
    zassert_equal(receiver.reassembler.Accept(frames[3], 0), Result::IGNORED,
        "The rest of a dropped packet is ignored");
}

ZTEST(smp_can_reassembler, test_start_frame_restarts_the_packet) {
    Receiver receiver;
    auto first = MakeFrames(MakePacket(30));
    auto second_packet = MakePacket(12);

    receiver.reassembler.Accept(first[0], 0);
    receiver.reassembler.Accept(first[1], 0);

    zassert_equal(receiver.Feed(MakeFrames(second_packet)), Result::COMPLETE);
    zassert_true(receiver.Holds(second_packet));
}

ZTEST(smp_can_reassembler, test_continuation_without_start_is_ignored) {
    Receiver receiver;
    auto frames = MakeFrames(MakePacket(30));

    zassert_equal(receiver.reassembler.Accept(frames[1], 0), Result::IGNORED);
    zassert_false(receiver.reassembler.IsActive());
}

ZTEST(smp_can_reassembler, test_start_frame_with_nonzero_sequence_is_dropped) {
    Receiver receiver;
    auto frames = MakeFrames(MakePacket(30));
    frames[0][0] |= 0x05;

    zassert_equal(receiver.reassembler.Accept(frames[0], 0), Result::DROPPED);
}

ZTEST(smp_can_reassembler, test_packet_larger_than_the_buffer_is_dropped) {
    std::array<uint8_t, 64> buffer{};
    SmpCanReassembler reassembler;
    reassembler.Attach(buffer);

    auto frames = MakeFrames(MakePacket(100));

    zassert_equal(reassembler.Accept(frames[0], 0), Result::IN_PROGRESS);
    zassert_equal(reassembler.Accept(frames[1], 0), Result::DROPPED,
        "Dropped as soon as the header announces more than the buffer holds");
}

ZTEST(smp_can_reassembler, test_short_intermediate_frame_is_dropped) {
    Receiver receiver;
    auto frames = MakeFrames(MakePacket(30));
    frames[1].pop_back();

    receiver.reassembler.Accept(frames[0], 0);

    zassert_equal(receiver.reassembler.Accept(frames[1], 0), Result::DROPPED);
}

ZTEST(smp_can_reassembler, test_data_beyond_the_packet_is_dropped) {
    Receiver receiver;
    auto frames = MakeFrames(MakePacket(2)); // 10 bytes: 7 + 3
    frames[1].push_back(0xAA);

    receiver.reassembler.Accept(frames[0], 0);

    zassert_equal(receiver.reassembler.Accept(frames[1], 0), Result::DROPPED);
}

ZTEST(smp_can_reassembler, test_empty_and_oversize_frames_are_rejected) {
    Receiver receiver;
    const Frame oversize(9, 0x80);

    zassert_equal(receiver.reassembler.Accept({}, 0), Result::IGNORED);
    zassert_equal(receiver.reassembler.Accept(oversize, 0), Result::IGNORED);
    zassert_equal(receiver.reassembler.Accept(Frame{SmpCanFrameFormat::START_FLAG}, 0), Result::DROPPED,
        "A start frame must carry data");
}

ZTEST(smp_can_reassembler, test_silent_source_times_out) {
    Receiver receiver;
    auto frames = MakeFrames(MakePacket(30));

    receiver.reassembler.Accept(frames[0], 1000);

    zassert_false(receiver.reassembler.IsExpired(1000 + TIMEOUT_MS));
    zassert_true(receiver.reassembler.IsExpired(1000 + TIMEOUT_MS + 1));
    zassert_equal(receiver.reassembler.Accept(frames[1], 1000 + TIMEOUT_MS + 1), Result::IGNORED,
        "A frame after the timeout does not continue the stale packet");
}

ZTEST(smp_can_reassembler, test_each_frame_extends_the_timeout) {
    Receiver receiver;
    auto packet = MakePacket(30);
    auto frames = MakeFrames(packet);
    Result result = Result::IGNORED;

    for(size_t i = 0; i < frames.size(); i++)
        result = receiver.reassembler.Accept(frames[i], static_cast<int64_t>(i) * TIMEOUT_MS);

    zassert_equal(result, Result::COMPLETE);
    zassert_true(receiver.Holds(packet));
}

ZTEST(smp_can_reassembler, test_can_fd_packets_round_trip_with_padding) {
    constexpr SmpCanFrameFormat can_fd(true);

    for(size_t body_size = 0; body_size <= 200; body_size++) {
        Receiver receiver(can_fd);
        auto packet = MakePacket(body_size);

        zassert_equal(receiver.Feed(MakeFrames(packet, can_fd)), Result::COMPLETE, "Body size %zu", body_size);
        zassert_true(receiver.Holds(packet), "Body size %zu", body_size);
    }
}

ZTEST(smp_can_reassembler, test_can_fd_padding_may_reach_past_a_full_buffer) {
    constexpr SmpCanFrameFormat can_fd(true);
    Receiver receiver(can_fd);
    auto packet = MakePacket(receiver.buffer.size() - SmpHeader::SIZE); // last frame: 17 bytes, sent as 20

    auto frames = MakeFrames(packet, can_fd);
    zassert_equal(frames.back().size(), 20);

    zassert_equal(receiver.Feed(frames), Result::COMPLETE);
    zassert_true(receiver.Holds(packet));
}

ZTEST(smp_can_reassembler, test_can_fd_last_frame_with_wrong_padding_is_dropped) {
    constexpr SmpCanFrameFormat can_fd(true);
    Receiver receiver(can_fd);
    auto frames = MakeFrames(MakePacket(68), can_fd); // last frame: 14 bytes, sent as 16
    frames[1].resize(20, 0);

    receiver.reassembler.Accept(frames[0], 0);

    zassert_equal(receiver.reassembler.Accept(frames[1], 0), Result::DROPPED);
}

ZTEST(smp_can_reassembler, test_classic_frames_do_not_continue_a_can_fd_packet) {
    constexpr SmpCanFrameFormat can_fd(true);
    Receiver receiver(can_fd);
    auto packet = MakePacket(200);
    auto fd_frames = MakeFrames(packet, can_fd);
    auto classic_frames = MakeFrames(packet);

    receiver.reassembler.Accept(fd_frames[0], 0);
    classic_frames[1][0] = fd_frames[1][0];

    zassert_equal(receiver.reassembler.Accept(classic_frames[1], 0), Result::DROPPED,
        "A short intermediate frame breaks the CAN FD format");
}

ZTEST(smp_can_reassembler, test_can_fd_frame_on_a_classic_receiver_is_rejected) {
    Receiver receiver;
    auto frames = MakeFrames(MakePacket(100), SmpCanFrameFormat(true));

    zassert_equal(receiver.reassembler.Accept(frames[0], 0), Result::IGNORED);
}
