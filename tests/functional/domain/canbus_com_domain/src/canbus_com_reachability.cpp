#include <cstdint>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "canbus_com_test_support.h"

using namespace canbus_com_test;

namespace {

// A second unit next to the scripted peer, with an ID above the unit's.
constexpr uint8_t OTHER_PEER_ID = 3;
constexpr uint32_t OTHER_PEER_UID = 0x33333333;
// CDMP major version 2.0; the unit under test speaks 1.0.
constexpr uint8_t OTHER_MAJOR_VERSION = 0x20;

Harness* harness_under_test = nullptr;

void* CanbusComReachabilitySetup() {
    return nullptr;
}

void CanbusComReachabilityBefore(void*) {
    harness_under_test = new Harness();
}

void CanbusComReachabilityAfter(void*) {
    delete harness_under_test;
    harness_under_test = nullptr;

    ResetLoopbackDevice();
}

} // namespace

ZTEST_SUITE(
    canbus_com_reachability,
    NULL,
    CanbusComReachabilitySetup,
    CanbusComReachabilityBefore,
    CanbusComReachabilityAfter,
    NULL
);

ZTEST(canbus_com_reachability, test_only_other_units_in_the_table_are_reachable) {
    Harness& harness = *harness_under_test;
    harness.BringOnline();
    harness.Settle();

    zassert_equal(harness.com->GetAddress(), OWN_ID, "SMP is bound to the claimed ID");
    zassert_true(harness.com->IsSmpBound());
    zassert_true(harness.com->IsReachable(PEER_ID), "The peer answered discovery");
    zassert_false(harness.com->IsReachable(OWN_ID), "Not the unit itself");
    zassert_false(harness.com->IsReachable(OTHER_PEER_ID), "Not a unit that is not in the table");
    zassert_false(harness.com->IsReachable(CdmpDevice::DEVICE_ID_UNASSIGNED));
    zassert_false(harness.com->IsReachable(CdmpDevice::DEVICE_ID_BROADCAST));

    harness.Join(OTHER_PEER_ID, OTHER_PEER_UID);

    zassert_true(harness.com->IsReachable(OTHER_PEER_ID), "A unit joins the table on its first heartbeat");
}

// Boot complete of a CAN-only unit waits for this (docs/dfu.md): the ID is claimed a few seconds after the start.
ZTEST(canbus_com_reachability, test_smp_is_bound_only_while_online_with_a_claimed_id) {
    Harness& harness = *harness_under_test;

    zassert_false(harness.com->IsSmpBound(), "Not started");

    zassert_true(harness.com->Start());

    zassert_true(harness.bus.Wait(MANAGEMENT, IsManagement(CdmpManagementMessageType::DISCOVERY_REQUEST)).has_value());
    zassert_false(harness.com->IsSmpBound(), "Discovering, with no ID yet");

    harness.SendAsPeer(
        MANAGEMENT,
        CdmpDiscoveryResponseMessage{ .device_id = PEER_ID, .uid = PEER_UID, .features = LOGGER_FEATURES }.ToCanFrame()
    );
    auto claim = harness.bus.Wait(MANAGEMENT, IsManagement(CdmpManagementMessageType::ID_CLAIM), STARTUP_TIMEOUT_MS);
    zassert_true(claim.has_value(), "The unit claims an ID after discovery");
    zassert_false(harness.com->IsSmpBound(), "Claiming, not yet answered");

    harness.SendAsPeer(
        MANAGEMENT,
        CdmpIdClaimResponseMessage{ .responding_device_id = PEER_ID,
            .claiming_device_id = OWN_ID,
            .claiming_device_uid = CdmpIdClaimRequestMessage::FromCanFrame(claim->data).uid,
            .result = CdmpIdClaimResult::ACCEPT }
            .ToCanFrame()
    );
    zassert_true(harness.WaitForStatus(CdmpDeviceStatus::ONLINE));

    // The transport binds from the status handler, right after the status changes.
    const int64_t deadline = k_uptime_get() + FRAME_TIMEOUT_MS;
    while(!harness.com->IsSmpBound()) {
        zassert_true(k_uptime_get() < deadline, "SMP binds to the claimed ID once the unit is online");
        k_msleep(5);
    }
    zassert_equal(harness.com->GetAddress(), OWN_ID);

    harness.Settle();
    zassert_true(harness.com->Stop());

    zassert_false(harness.com->IsSmpBound(), "A stopped node unbinds SMP");
}

// The access point asks CDMP whether a unit is reachable, and CDMP's table has no entry for a unit
// whose ID claim it refused for its version, so the access point cannot forward to it.
ZTEST(canbus_com_reachability, test_unit_on_another_major_version_is_not_reachable) {
    Harness& harness = *harness_under_test;
    harness.BringOnline();
    harness.Settle();

    harness.SendAsPeer(
        MANAGEMENT,
        CdmpIdClaimRequestMessage{
            .claiming_device_id = OTHER_PEER_ID, .uid = OTHER_PEER_UID, .protocol_version = OTHER_MAJOR_VERSION }
            .ToCanFrame()
    );
    harness.SendAsPeer(HEARTBEAT, HeartbeatFrame(OTHER_PEER_ID, 0, LOGGER_FEATURES));

    // A heartbeat of a unit that is not in the table makes the unit rediscover the network, which
    // also says that the heartbeat has been processed.
    zassert_true(
        harness.bus.Wait(MANAGEMENT, IsManagement(CdmpManagementMessageType::DISCOVERY_REQUEST)).has_value(),
        "The heartbeat of an unknown unit makes the unit send a discovery request"
    );

    zassert_false(harness.IsInDeviceTable(OTHER_PEER_ID), "The refused claim leaves no pending claim to join on");
    zassert_false(harness.com->IsReachable(OTHER_PEER_ID));

    // Nothing else changes: the unit stays online and keeps its address and its other peer.
    zassert_equal(harness.com->GetNetworkInfo()->GetDeviceInfo().status, CdmpDeviceStatus::ONLINE);
    zassert_equal(harness.com->GetAddress(), OWN_ID);
    zassert_true(harness.com->IsReachable(PEER_ID));
}

// The unit that is refused for its version is the one that cannot be reached over CAN.
ZTEST(canbus_com_reachability, test_unit_refused_for_its_version_has_no_address_and_reaches_no_one) {
    Harness& harness = *harness_under_test;
    zassert_true(harness.com->Start());

    zassert_true(harness.bus.Wait(MANAGEMENT, IsManagement(CdmpManagementMessageType::DISCOVERY_REQUEST)).has_value());
    harness.SendAsPeer(
        MANAGEMENT,
        CdmpDiscoveryResponseMessage{ .device_id = PEER_ID, .uid = PEER_UID, .features = LOGGER_FEATURES }.ToCanFrame()
    );

    auto claim = harness.bus.Wait(MANAGEMENT, IsManagement(CdmpManagementMessageType::ID_CLAIM), STARTUP_TIMEOUT_MS);
    zassert_true(claim.has_value(), "The unit claims an ID after discovery");
    const auto claim_message = CdmpIdClaimRequestMessage::FromCanFrame(claim->data);
    zassert_equal(claim_message.claiming_device_id, OWN_ID);

    // The peer holds the lowest ID and speaks another major version.
    harness.SendAsPeer(
        MANAGEMENT,
        CdmpIdClaimResponseMessage{ .responding_device_id = PEER_ID,
            .claiming_device_id = OWN_ID,
            .claiming_device_uid = claim_message.uid,
            .result = CdmpIdClaimResult::VERSION_INCOMPATIBLE }
            .ToCanFrame()
    );

    zassert_true(harness.WaitForStatus(CdmpDeviceStatus::VERSION_MISMATCH));

    // The network service stops itself from the CDMP work queue and drops its table.
    const int64_t deadline = k_uptime_get() + FRAME_TIMEOUT_MS;
    while(harness.com->GetNetworkInfo()->GetNetworkDeviceCount() != 0) {
        zassert_true(k_uptime_get() < deadline, "The refused unit forgets the network");
        k_msleep(5);
    }

    // The ID is assigned only when a claim succeeds, so the unit has none to bind SMP to.
    zassert_equal(harness.com->GetNetworkInfo()->GetDeviceInfo().device_id, CdmpDevice::DEVICE_ID_UNASSIGNED);
    zassert_equal(harness.com->GetAddress(), 0);
    zassert_false(harness.com->IsSmpBound(), "A unit refused for its version cannot be managed over CAN");
    zassert_false(harness.com->IsReachable(PEER_ID));
    zassert_false(harness.com->IsReachable(OWN_ID));
}
