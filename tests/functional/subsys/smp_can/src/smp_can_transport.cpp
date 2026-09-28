#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "smp_can_test_support.h"

using namespace smp_can_test;

struct smp_can_transport_fixture {
    Harness* harness;
};

static void* SmpCanTransportSetup() {
    static smp_can_transport_fixture fixture{};

    return &fixture;
}

static void SmpCanTransportBefore(void* data) {
    static_cast<smp_can_transport_fixture*>(data)->harness = new Harness();
}

static void SmpCanTransportAfter(void* data) {
    auto* fixture = static_cast<smp_can_transport_fixture*>(data);
    delete fixture->harness;
    fixture->harness = nullptr;

    ResetLoopbackDevice();
}

ZTEST_SUITE(smp_can_transport, NULL, SmpCanTransportSetup, SmpCanTransportBefore, SmpCanTransportAfter, NULL);

ZTEST_F(smp_can_transport, test_echo_round_trip) {
    Harness& harness = *fixture->harness;

    harness.SendPacket(FIRST_PEER, MakeEchoRequest("hello", 1));

    zassert_true(IsEchoResponse(harness.peers.WaitPacket(FIRST_PEER), "hello", 1));
    zassert_equal(harness.peers.GetForeignSourceCount(), 0, "Responses must carry the bound address as source");
}

ZTEST_F(smp_can_transport, test_long_echo_spans_many_frames) {
    Harness& harness = *fixture->harness;
    const std::string text = LongText(200);
    const Bytes request = MakeEchoRequest(text, 2);

    zassert_true(SmpCanFramer::GetFrameCount(request.size()) > 30);

    harness.SendPacket(FIRST_PEER, request);

    zassert_true(IsEchoResponse(harness.peers.WaitPacket(FIRST_PEER), text, 2));
}

ZTEST_F(smp_can_transport, test_lost_frame_drops_the_request) {
    Harness& harness = *fixture->harness;
    const std::string text = LongText(40);
    auto frames = MakeFrames(MakeEchoRequest(text, 3));
    frames.erase(frames.begin() + 2);

    for(const auto& frame : frames)
        harness.SendFrame(FIRST_PEER, OWN_ADDRESS, frame);

    zassert_false(harness.peers.WaitPacket(FIRST_PEER, 300).has_value());
    zassert_true(harness.transport->GetRxDroppedCount() >= 1);

    harness.SendPacket(FIRST_PEER, MakeEchoRequest(text, 4));

    zassert_true(IsEchoResponse(harness.peers.WaitPacket(FIRST_PEER), text, 4),
        "The retried request must succeed");
}

ZTEST_F(smp_can_transport, test_interleaved_sources_are_reassembled_separately) {
    Harness& harness = *fixture->harness;
    const uint8_t first = FIRST_PEER;
    const uint8_t second = FIRST_PEER + 1;
    const std::string first_text = LongText(50);
    const std::string second_text = LongText(30);

    auto first_frames = MakeFrames(MakeEchoRequest(first_text, 5));
    auto second_frames = MakeFrames(MakeEchoRequest(second_text, 6));

    for(size_t i = 0; i < std::max(first_frames.size(), second_frames.size()); i++) {
        if(i < first_frames.size())
            harness.SendFrame(first, OWN_ADDRESS, first_frames[i]);
        if(i < second_frames.size())
            harness.SendFrame(second, OWN_ADDRESS, second_frames[i]);
    }

    zassert_true(IsEchoResponse(harness.peers.WaitPacket(first), first_text, 5));
    zassert_true(IsEchoResponse(harness.peers.WaitPacket(second), second_text, 6));
}

ZTEST_F(smp_can_transport, test_source_without_a_free_slot_is_dropped) {
    Harness& harness = *fixture->harness;
    const uint8_t third = FIRST_PEER + 2;
    const std::string text = LongText(30);

    auto first_frames = MakeFrames(MakeEchoRequest(text, 7));
    auto second_frames = MakeFrames(MakeEchoRequest(text, 8));

    harness.SendFrame(FIRST_PEER, OWN_ADDRESS, first_frames[0]);
    harness.SendFrame(FIRST_PEER + 1, OWN_ADDRESS, second_frames[0]);

    const uint32_t dropped_before = harness.transport->GetRxDroppedCount();
    harness.SendPacket(third, MakeEchoRequest(text, 9));

    // Shorter than the slot timeout, so the first two packets stay in progress.
    k_msleep(20);
    zassert_true(harness.transport->GetRxDroppedCount() > dropped_before, "Both slots are busy");

    for(size_t i = 1; i < first_frames.size(); i++) {
        harness.SendFrame(FIRST_PEER, OWN_ADDRESS, first_frames[i]);
        harness.SendFrame(FIRST_PEER + 1, OWN_ADDRESS, second_frames[i]);
    }

    zassert_true(IsEchoResponse(harness.peers.WaitPacket(FIRST_PEER), text, 7));
    zassert_true(IsEchoResponse(harness.peers.WaitPacket(FIRST_PEER + 1), text, 8));
    zassert_false(harness.peers.WaitPacket(third, 0).has_value(), "The dropped request got no response");

    harness.SendPacket(third, MakeEchoRequest(text, 10));

    zassert_true(IsEchoResponse(harness.peers.WaitPacket(third), text, 10), "Slots are free again");
}

ZTEST_F(smp_can_transport, test_stale_slot_is_reclaimed_after_the_timeout) {
    Harness& harness = *fixture->harness;
    const std::string text = LongText(30);

    harness.SendFrame(FIRST_PEER, OWN_ADDRESS, MakeFrames(MakeEchoRequest(text, 11))[0]);
    harness.SendFrame(FIRST_PEER + 1, OWN_ADDRESS, MakeFrames(MakeEchoRequest(text, 12))[0]);

    k_msleep(CONFIG_EERIE_LEAP_SMP_CAN_RX_TIMEOUT_MS + 50);

    harness.SendPacket(FIRST_PEER + 2, MakeEchoRequest(text, 13));

    zassert_true(IsEchoResponse(harness.peers.WaitPacket(FIRST_PEER + 2), text, 13));
}

ZTEST_F(smp_can_transport, test_forwarded_request_and_its_response) {
    Harness& harness = *fixture->harness;
    auto sink = std::make_shared<RecordingSink>();
    harness.transport->SetResponseSink(sink);

    const Bytes request = MakeEchoRequest("forwarded", 14);
    zassert_true(harness.transport->Forward(FIRST_PEER + 3, ToSmpPacket(request)));

    auto received = harness.peers.WaitPacket(FIRST_PEER + 3);
    zassert_true(received.has_value());
    zassert_true(*received == request, "The target receives the packet unchanged");

    const Bytes response = MakePacket(SmpOperation::WRITE_RESPONSE, 0, 0, 14, {0xA1, 0x61, 'r', 0x61, 'x'});
    harness.SendPacket(FIRST_PEER + 3, response);

    auto delivered = sink->Wait();
    zassert_true(delivered.has_value());
    zassert_equal(delivered->first, FIRST_PEER + 3);
    zassert_true(delivered->second == response);
}

ZTEST_F(smp_can_transport, test_response_without_a_sink_is_dropped) {
    Harness& harness = *fixture->harness;
    const uint32_t dropped_before = harness.transport->GetRxDroppedCount();

    harness.SendPacket(FIRST_PEER, MakePacket(SmpOperation::READ_RESPONSE, 0, 0, 1, {0xA0}));
    k_msleep(50);

    zassert_equal(harness.transport->GetRxDroppedCount(), dropped_before + 1);
}

ZTEST_F(smp_can_transport, test_paced_rate_stays_within_the_share) {
    Harness& harness = *fixture->harness;
    const Bytes body(1000, 0x5A);
    const Bytes packet = MakePacket(SmpOperation::WRITE, 64, 0, 15, body);
    const size_t frame_count = SmpCanFramer::GetFrameCount(packet.size());

    const uint32_t frames_per_second = SmpCanPacer(
        harness.format.GetFrameTimeNs(BITRATE), BUS_SHARE_PERCENT, 1).GetFramesPerSecond();
    const int64_t minimum_ms =
        static_cast<int64_t>(frame_count - CONFIG_EERIE_LEAP_SMP_CAN_TX_BURST_FRAMES) * 1000 / frames_per_second;

    const int64_t start_ms = k_uptime_get();
    zassert_true(harness.transport->Forward(FIRST_PEER, ToSmpPacket(packet)));

    auto received = harness.peers.WaitPacket(FIRST_PEER, 5000);
    zassert_true(received.has_value());
    zassert_true(*received == packet);
    zassert_equal(harness.peers.GetFrameCount(), frame_count);

    // One tick of slack for the uptime granularity.
    const int64_t elapsed_ms = harness.peers.GetLastFrameMs() - start_ms;
    const int64_t tick_ms = 1000 / CONFIG_SYS_CLOCK_TICKS_PER_SEC;
    zassert_true(elapsed_ms + tick_ms >= minimum_ms,
        "%zu frames took %lld ms, the share allows no less than %lld ms",
        frame_count, static_cast<long long>(elapsed_ms), static_cast<long long>(minimum_ms));
}

ZTEST_F(smp_can_transport, test_unbound_transport_ignores_requests) {
    Harness& harness = *fixture->harness;

    harness.transport->Unbind();
    zassert_equal(harness.transport->GetAddress(), 0);

    harness.SendPacket(FIRST_PEER, MakeEchoRequest("unbound", 16));
    zassert_false(harness.peers.WaitPacket(FIRST_PEER, 300).has_value());
    zassert_false(harness.transport->Forward(FIRST_PEER, ToSmpPacket(MakeEchoRequest("x", 1))));
}

ZTEST_F(smp_can_transport, test_rebind_moves_to_the_new_address) {
    Harness& harness = *fixture->harness;
    constexpr uint8_t new_address = OWN_ADDRESS + 1;

    zassert_true(harness.transport->Bind(new_address));
    zassert_equal(harness.transport->GetAddress(), new_address);

    harness.SendPacket(FIRST_PEER, MakeEchoRequest("old", 17), OWN_ADDRESS);
    zassert_false(harness.peers.WaitPacket(FIRST_PEER, 300).has_value(), "The old address is released");

    harness.SendPacket(FIRST_PEER, MakeEchoRequest("new", 18), new_address);
    auto response = harness.peers.WaitPacket(FIRST_PEER);
    zassert_true(IsEchoResponse(response, "new", 18));
    zassert_equal(harness.peers.GetLastSource(), new_address, "The response comes from the new address");
}

ZTEST_F(smp_can_transport, test_invalid_addresses_are_rejected) {
    Harness& harness = *fixture->harness;

    zassert_false(harness.transport->Bind(0x00));
    zassert_false(harness.transport->Bind(0xFF));
    zassert_equal(harness.transport->GetAddress(), OWN_ADDRESS, "A rejected bind keeps the current address");

    zassert_false(harness.transport->Forward(0x00, ToSmpPacket(MakeEchoRequest("x", 1))));
    zassert_false(harness.transport->Forward(0xFF, ToSmpPacket(MakeEchoRequest("x", 1))));
    zassert_false(harness.transport->Forward(OWN_ADDRESS, ToSmpPacket(MakeEchoRequest("x", 1))));
}
