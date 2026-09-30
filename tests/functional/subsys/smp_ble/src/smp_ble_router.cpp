#include <zephyr/ztest.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>

#include "smp_ble_test_support.h"

using namespace smp_ble_test;

ZTEST_SUITE(smp_ble_router, NULL, NULL, NULL, NULL, NULL);

ZTEST(smp_ble_router, test_local_route_reaches_the_smp_server) {
    RouterHarness harness;

    harness.router->OnReceive(Routed(0, MakeEchoRequest("hello", 11)));

    auto packets = harness.link->WaitForPackets(1);
    zassert_equal(packets.size(), 1);
    ExpectEchoResponse(packets[0], 0, "hello", 11);
    zassert_true(harness.forwarder->forwarded.empty());
}

ZTEST(smp_ble_router, test_own_address_is_served_locally) {
    RouterHarness harness;
    harness.forwarder->address = 5;
    harness.forwarder->reachable = {7};

    harness.router->OnReceive(Routed(5, MakeEchoRequest("self", 12)));

    auto packets = harness.link->WaitForPackets(1);
    zassert_equal(packets.size(), 1);
    ExpectEchoResponse(packets[0], 5, "self", 12);
    zassert_true(harness.forwarder->forwarded.empty());
}

ZTEST(smp_ble_router, test_packets_span_writes_and_notifications) {
    RouterHarness harness;
    const std::string text = LongText(120);

    // One byte per write splits the route and the SMP header too.
    for(uint8_t byte : Routed(0, MakeEchoRequest(text, 13)))
        harness.router->OnReceive({&byte, 1});

    auto packets = harness.link->WaitForPackets(1);
    zassert_equal(packets.size(), 1);
    ExpectEchoResponse(packets[0], 0, text, 13);

    const auto sizes = harness.link->Sizes();
    zassert_true(sizes.size() > 1);
    zassert_true(std::ranges::all_of(sizes, [](size_t size) { return size <= 20; }));
}

ZTEST(smp_ble_router, test_one_write_may_hold_several_packets) {
    RouterHarness harness;

    Bytes data = Routed(0, MakeEchoRequest("one", 1));
    const Bytes second = Routed(0, MakeEchoRequest("two", 2));
    data.insert(data.end(), second.begin(), second.end());

    harness.router->OnReceive(data);

    auto packets = harness.link->WaitForPackets(2);
    zassert_equal(packets.size(), 2);
    ExpectEchoResponse(packets[0], 0, "one", 1);
    ExpectEchoResponse(packets[1], 0, "two", 2);
}

ZTEST(smp_ble_router, test_peer_route_is_forwarded_and_its_response_notified) {
    RouterHarness harness;
    harness.forwarder->address = 5;
    harness.forwarder->reachable = {7};

    const Bytes request = MakeEchoRequest("peer", 14);
    harness.router->OnReceive(Routed(7, request));

    zassert_equal(harness.forwarder->forwarded.size(), 1);
    zassert_equal(harness.forwarder->forwarded[0].first, 7);
    zassert_true(harness.forwarder->forwarded[0].second == request);

    Bytes body = {0xA1, 0x61, 'r'};
    const Bytes text = CborText("peer");
    body.insert(body.end(), text.begin(), text.end());
    const Bytes response = MakePacket(SmpOperation::WRITE_RESPONSE, 0, 0, 14, body);
    harness.router->OnResponse(7, ToSmpPacket(response));

    auto packets = harness.link->WaitForPackets(1);
    zassert_equal(packets.size(), 1);
    zassert_equal(packets[0].route, 7);
    zassert_true(packets[0].packet == response);
}

ZTEST(smp_ble_router, test_refused_forward_is_dropped) {
    RouterHarness harness;
    harness.forwarder->reachable = {7};
    harness.forwarder->accept = false;

    harness.router->OnReceive(Routed(7, MakeEchoRequest("lost", 15)));

    zassert_equal(harness.router->GetRxDroppedCount(), 1);
    k_msleep(50);
    zassert_true(harness.link->Stream().empty());
}

ZTEST(smp_ble_router, test_unreachable_destination_gets_enoent) {
    for(bool has_forwarder : {true, false}) {
        RouterHarness harness(has_forwarder);

        harness.router->OnReceive(Routed(9, MakeEchoRequest("nobody", 16, SmpOperation::READ)));

        auto packets = harness.link->WaitForPackets(1);
        zassert_equal(packets.size(), 1);
        zassert_equal(packets[0].route, 9);
        zassert_equal(packets[0].header.operation, SmpOperation::READ_RESPONSE);
        zassert_equal(packets[0].header.version, 1);
        zassert_equal(packets[0].header.group, 0);
        zassert_equal(packets[0].header.command_id, 0);
        zassert_equal(packets[0].header.sequence, 16);
        zassert_true(FindInt(packets[0].Body(), "rc") == MGMT_ERR_ENOENT);

        if(has_forwarder)
            zassert_true(harness.forwarder->forwarded.empty());
    }
}

ZTEST(smp_ble_router, test_busy_link_resumes_when_a_notification_leaves) {
    RouterHarness harness;
    harness.link->SetBudget(1);

    const std::string text = LongText(60);
    harness.router->OnReceive(Routed(0, MakeEchoRequest(text, 17)));

    k_msleep(100);
    zassert_equal(harness.link->Sizes().size(), 1, "Only one notification fits");

    harness.link->SetBudget(-1);
    harness.router->OnNotificationSent();

    auto packets = harness.link->WaitForPackets(1);
    zassert_equal(packets.size(), 1);
    ExpectEchoResponse(packets[0], 0, text, 17);
    zassert_equal(harness.router->GetTxDroppedCount(), 0);
}

ZTEST(smp_ble_router, test_disconnect_drops_a_partial_request) {
    RouterHarness harness;

    const Bytes stale = Routed(0, MakeEchoRequest("stale", 18));
    harness.router->OnReceive(std::span(stale).first(6));
    harness.router->OnDisconnected();

    harness.router->OnReceive(Routed(0, MakeEchoRequest("fresh", 19)));

    auto packets = harness.link->WaitForPackets(1);
    zassert_equal(packets.size(), 1);
    ExpectEchoResponse(packets[0], 0, "fresh", 19);
}

ZTEST(smp_ble_router, test_disconnect_drops_pending_responses) {
    {
        RouterHarness harness;
        harness.link->SetBudget(0);

        harness.router->OnReceive(Routed(0, MakeEchoRequest("late", 20)));
        k_msleep(100);

        harness.router->OnDisconnected();
        harness.link->SetBudget(-1);
        harness.router->OnNotificationSent();
        k_msleep(100);

        zassert_true(harness.link->Stream().empty());
        zassert_equal(harness.router->GetTxDroppedCount(), 1);
    }

    zassert_equal(CountFreePackets(), CONFIG_MCUMGR_TRANSPORT_NETBUF_COUNT);
}

ZTEST(smp_ble_router, test_stalled_request_times_out) {
    RouterHarness harness;

    const Bytes stale = Routed(0, MakeEchoRequest("stale", 21));
    harness.router->OnReceive(std::span(stale).first(12));
    k_msleep(CONFIG_EERIE_LEAP_SMP_BLE_RX_TIMEOUT_MS + 50);

    harness.router->OnReceive(Routed(0, MakeEchoRequest("fresh", 22)));

    auto packets = harness.link->WaitForPackets(1);
    zassert_equal(packets.size(), 1);
    ExpectEchoResponse(packets[0], 0, "fresh", 22);
    zassert_equal(harness.router->GetRxDroppedCount(), 1);
}

ZTEST(smp_ble_router, test_oversized_packets_and_responses_are_skipped) {
    RouterHarness harness;

    // Longer than a pool buffer; its body is skipped, not parsed as packets.
    const Bytes oversized = MakePacket(SmpOperation::WRITE, 0, 0, 23, Bytes(CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE, 0xA1));
    const Bytes routed = Routed(0, oversized);
    for(size_t offset = 0; offset < routed.size(); offset += 100)
        harness.router->OnReceive(std::span(routed).subspan(offset, std::min<size_t>(100, routed.size() - offset)));

    harness.router->OnReceive(Routed(0, MakePacket(SmpOperation::READ_RESPONSE, 0, 0, 24, {0xA0})));
    harness.router->OnReceive(Routed(0, MakeEchoRequest("after", 25)));

    auto packets = harness.link->WaitForPackets(1);
    zassert_equal(packets.size(), 1);
    ExpectEchoResponse(packets[0], 0, "after", 25);
    zassert_equal(harness.router->GetRxDroppedCount(), 2);
}

ZTEST(smp_ble_router, test_info_reports_version_packet_size_and_requests_in_flight) {
    const auto info = SmpBleRouter::GetInfo();

    zassert_equal(info[0], SmpBleRouter::PROTOCOL_VERSION);
    zassert_equal(info[1] | (info[2] << 8), CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE);
    zassert_equal(info[3], CONFIG_EERIE_LEAP_SMP_BLE_REQUESTS_IN_FLIGHT);
}
