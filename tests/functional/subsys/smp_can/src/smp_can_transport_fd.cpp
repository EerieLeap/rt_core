#include <memory>
#include <string>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "smp_can_test_support.h"

using namespace smp_can_test;

namespace {

constexpr uint32_t DATA_BITRATE = 2000000;
constexpr SmpCanFrameFormat CAN_FD(true);

} // namespace

struct smp_can_transport_fd_fixture {
    Harness* harness;
};

static void* SmpCanTransportFdSetup() {
    static smp_can_transport_fd_fixture fixture{};

    return &fixture;
}

static void SmpCanTransportFdBefore(void* data) {
    static_cast<smp_can_transport_fd_fixture*>(data)->harness = new Harness(CanbusType::CANFD, DATA_BITRATE);
}

static void SmpCanTransportFdAfter(void* data) {
    auto* fixture = static_cast<smp_can_transport_fd_fixture*>(data);
    delete fixture->harness;
    fixture->harness = nullptr;

    ResetLoopbackDevice();
}

ZTEST_SUITE(smp_can_transport_fd, NULL, SmpCanTransportFdSetup, SmpCanTransportFdBefore, SmpCanTransportFdAfter, NULL);

ZTEST_F(smp_can_transport_fd, test_echo_round_trip_uses_can_fd_frames) {
    Harness& harness = *fixture->harness;

    harness.SendPacket(FIRST_PEER, MakeEchoRequest("hello", 1));

    zassert_true(IsEchoResponse(harness.peers.WaitPacket(FIRST_PEER), "hello", 1));
    zassert_true(harness.peers.GetFrameCount() > 0);
    zassert_equal(harness.peers.GetCanFdFrameCount(), harness.peers.GetFrameCount(),
        "A CAN FD channel sends CAN FD frames");
}

ZTEST_F(smp_can_transport_fd, test_long_echo_uses_64_byte_frames) {
    Harness& harness = *fixture->harness;
    const std::string text = LongText(200);
    const Bytes request = MakeEchoRequest(text, 2);

    zassert_equal(SmpCanFramer::GetFrameCount(request.size(), CAN_FD), 4);

    harness.SendPacket(FIRST_PEER, request);

    auto response = harness.peers.WaitPacket(FIRST_PEER);
    zassert_true(IsEchoResponse(response, text, 2));
    zassert_equal(harness.peers.GetFrameCount(), SmpCanFramer::GetFrameCount(response->size(), CAN_FD));
}

ZTEST_F(smp_can_transport_fd, test_interleaved_sources_are_reassembled_separately) {
    Harness& harness = *fixture->harness;
    const std::string first_text = LongText(150);
    const std::string second_text = LongText(90);

    auto first_frames = MakeFrames(MakeEchoRequest(first_text, 3), CAN_FD);
    auto second_frames = MakeFrames(MakeEchoRequest(second_text, 4), CAN_FD);

    for(size_t i = 0; i < std::max(first_frames.size(), second_frames.size()); i++) {
        if(i < first_frames.size())
            harness.SendFrame(FIRST_PEER, OWN_ADDRESS, first_frames[i]);
        if(i < second_frames.size())
            harness.SendFrame(FIRST_PEER + 1, OWN_ADDRESS, second_frames[i]);
    }

    zassert_true(IsEchoResponse(harness.peers.WaitPacket(FIRST_PEER), first_text, 3));
    zassert_true(IsEchoResponse(harness.peers.WaitPacket(FIRST_PEER + 1), second_text, 4));
}

ZTEST_F(smp_can_transport_fd, test_forwarded_packet_fills_the_buffer_in_16_frames) {
    Harness& harness = *fixture->harness;
    const Bytes packet = MakePacket(SmpOperation::WRITE, 64, 0, 5, Bytes(1000, 0x5A));

    zassert_true(harness.transport->Forward(FIRST_PEER, ToSmpPacket(packet)));

    auto received = harness.peers.WaitPacket(FIRST_PEER);
    zassert_true(received.has_value());
    zassert_true(*received == packet);
    zassert_equal(harness.peers.GetFrameCount(), 16);
}

ZTEST_F(smp_can_transport_fd, test_response_to_a_forwarded_request_reaches_the_sink) {
    Harness& harness = *fixture->harness;
    auto sink = std::make_shared<RecordingSink>();
    harness.transport->SetResponseSink(sink);

    const Bytes response = MakePacket(SmpOperation::WRITE_RESPONSE, 0, 0, 6, Bytes(100, 0xA5));
    harness.SendPacket(FIRST_PEER + 3, response);

    auto delivered = sink->Wait();
    zassert_true(delivered.has_value());
    zassert_equal(delivered->first, FIRST_PEER + 3);
    zassert_true(delivered->second == response, "The CAN FD padding is not part of the packet");
}
