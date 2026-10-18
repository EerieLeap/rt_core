#include <array>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "cdmp_state_test_support.h"

using namespace cdmp_state_test;

namespace {

// A wait on a timer may end a tick late, and QEMU without icount adds scheduling jitter.
constexpr int TIMING_TOLERANCE_BELOW_MS = 10;
constexpr int TIMING_TOLERANCE_ABOVE_MS = 60;

// A device ID that is neither the unit's nor the peer's.
constexpr uint8_t OTHER_SOURCE_ID = 7;

/** @brief Expects the three resends of @p change, 50, 100 and 200 ms apart. */
void ExpectResends(Harness& harness, const RecordedFrame& change) {
    int64_t previous_ms = change.received_ms;
    for(int backoff : { 50, 100, 200 }) {
        auto resend = harness.bus.Wait(STATE_CHANGE, IsFrom(OWN_ID));
        zassert_true(resend.has_value(), "Resent after %d ms", backoff);
        zassert_true(resend->data == change.data, "A resend carries the same version and data");

        const int64_t gap = resend->received_ms - previous_ms;
        zassert_true(
            gap >= backoff - TIMING_TOLERANCE_BELOW_MS && gap <= backoff + TIMING_TOLERANCE_ABOVE_MS,
            "Resent %lld ms after the previous frame, expected %d", static_cast<long long>(gap), backoff
        );
        previous_ms = resend->received_ms;
    }
}

} // namespace

struct cdmp_state_sync_fixture {
    Harness* harness;
};

static void* CdmpStateSetup() {
    static cdmp_state_sync_fixture fixture{};

    return &fixture;
}

static void CdmpStateBefore(void* data) {
    static_cast<cdmp_state_sync_fixture*>(data)->harness = new Harness();
}

static void CdmpStateAfter(void* data) {
    auto* fixture = static_cast<cdmp_state_sync_fixture*>(data);
    delete fixture->harness;
    fixture->harness = nullptr;

    ResetLoopbackDevice();
}

ZTEST_SUITE(cdmp_state_sync, NULL, CdmpStateSetup, CdmpStateBefore, CdmpStateAfter, NULL);

ZTEST_F(cdmp_state_sync, test_online_announces_records_and_requests_a_sync) {
    Harness& harness = *fixture->harness;
    // Set before the start, so it waits for the first ONLINE.
    zassert_true(harness.State()->SetLocalState(TYPE_A, Bytes{ 0x01, 0x02 }));

    harness.BringOnline();

    auto notification = harness.WaitForNotification();
    zassert_true(notification.has_value(), "The unit announces its records on going online");
    zassert_equal(notification->source_device_id, OWN_ID);
    zassert_equal(notification->version, 1, "The first change of the unit is version 1");
    zassert_equal(notification->state_type, TYPE_A);
    zassert_true(notification->data == CdmpStateData(Bytes{ 0x01, 0x02 }));

    zassert_true(
        harness.bus.Wait(COMMAND, IsSyncRequestTo(CdmpDevice::DEVICE_ID_BROADCAST)).has_value(),
        "and asks every other unit for theirs"
    );

    auto heartbeat = harness.bus.Wait(HEARTBEAT, IsFrom(OWN_ID));
    zassert_true(heartbeat.has_value());
    zassert_equal(
        CdmpHeartbeatMessage::FromCanFrame(heartbeat->data).state_digest, Digest({ { TYPE_A, 1, { 0x01, 0x02 } } }),
        "The heartbeat carries the digest of the unit's records"
    );
}

ZTEST_F(cdmp_state_sync, test_acknowledgement_stops_retransmission) {
    Harness& harness = *fixture->harness;
    harness.BringOnline();
    harness.Settle();

    zassert_true(harness.State()->SetLocalState(TYPE_A, Bytes{ 0x01 }));

    auto notification = harness.WaitForNotification();
    zassert_true(notification.has_value());
    harness.SendAsPeer(STATE_CHANGE_RESPONSE, AcknowledgementFrame(PEER_ID, OWN_ID, notification->version));

    zassert_false(harness.WaitForNotification(500).has_value(), "Every online peer has acknowledged the change");
}

ZTEST_F(cdmp_state_sync, test_unacknowledged_change_is_resent_three_times) {
    Harness& harness = *fixture->harness;
    harness.BringOnline();
    harness.Settle();

    zassert_true(harness.State()->SetLocalState(TYPE_A, Bytes{ 0x01 }));

    auto first = harness.bus.Wait(STATE_CHANGE, IsFrom(OWN_ID));
    zassert_true(first.has_value());

    ExpectResends(harness, first.value());

    zassert_false(harness.WaitForNotification(500).has_value(), "The digest takes over after the third resend");
}

ZTEST_F(cdmp_state_sync, test_acknowledgement_of_another_change_does_not_stop_resending) {
    Harness& harness = *fixture->harness;
    harness.BringOnline();
    harness.Settle();

    zassert_true(harness.State()->SetLocalState(TYPE_A, Bytes{ 0x01 }));
    auto notification = harness.WaitForNotification();
    zassert_true(notification.has_value());
    const uint8_t version = notification->version;

    harness.SendAsPeer(STATE_CHANGE_RESPONSE, AcknowledgementFrame(PEER_ID, OWN_ID, static_cast<uint8_t>(version - 1)));

    auto resend = harness.WaitForNotification();
    zassert_true(resend.has_value(), "An acknowledgement of an older version is not one of this change");
    zassert_equal(resend->version, version);

    harness.SendAsPeer(STATE_CHANGE_RESPONSE, AcknowledgementFrame(PEER_ID, OTHER_SOURCE_ID, version));

    resend = harness.WaitForNotification();
    zassert_true(resend.has_value(), "Nor is one of another unit's notification with the same version");
    zassert_equal(resend->version, version);

    harness.SendAsPeer(STATE_CHANGE_RESPONSE, AcknowledgementFrame(PEER_ID, OWN_ID, version));

    zassert_false(harness.WaitForNotification(500).has_value(), "The peer's acknowledgement of this change stops them");
}

ZTEST_F(cdmp_state_sync, test_newer_change_replaces_the_resends_of_the_previous_one) {
    Harness& harness = *fixture->harness;
    harness.BringOnline();
    harness.Settle();

    zassert_true(harness.State()->SetLocalState(TYPE_A, Bytes{ 0x01 }));
    auto first = harness.WaitForNotification();
    zassert_true(first.has_value());
    auto resend = harness.WaitForNotification();
    zassert_true(resend.has_value());
    zassert_equal(resend->version, first->version, "The first change was resent once");

    zassert_true(harness.State()->SetLocalState(TYPE_A, Bytes{ 0x02 }));

    auto change = harness.bus.Wait(STATE_CHANGE, IsFrom(OWN_ID));
    zassert_true(change.has_value());
    const auto message = CdmpStateChangeMessage::FromCanFrame(change->data);
    zassert_equal(message.version, first->version + 1, "The next frame carries the newer change");
    zassert_true(message.data == CdmpStateData(Bytes{ 0x02 }));

    // Three resends again, timed from the newer change.
    ExpectResends(harness, change.value());

    zassert_false(harness.WaitForNotification(500).has_value(), "and no more");
}

ZTEST_F(cdmp_state_sync, test_unicast_sync_request_replays_records_and_answers) {
    Harness& harness = *fixture->harness;
    zassert_true(harness.State()->SetLocalState(TYPE_A, Bytes{ 0x01 }));
    zassert_true(harness.State()->SetLocalState(TYPE_B, Bytes{ 0x02 }));
    harness.State()->ClearLocalState(TYPE_B);
    zassert_true(harness.State()->SetLocalState(TYPE_B + 1, Bytes{ 0x03 }));

    harness.BringOnline();
    for(uint8_t version : { 1, 3, 4 }) {
        auto notification = harness.WaitForNotification();
        zassert_true(notification.has_value());
        zassert_equal(notification->version, version);
        harness.SendAsPeer(STATE_CHANGE_RESPONSE, AcknowledgementFrame(PEER_ID, OWN_ID, version));
    }
    harness.Settle();
    harness.bus.Clear();

    harness.SendAsPeer(COMMAND, SyncRequestFrame(OWN_ID, 0x42));

    std::vector<uint32_t> replay_sequences;
    for(uint8_t state_type : { TYPE_A, static_cast<uint8_t>(TYPE_B + 1) }) {
        auto frame = harness.bus.Wait(STATE_CHANGE, IsFrom(OWN_ID));
        zassert_true(frame.has_value(), "The unit replays every record it holds");

        // The withdrawal of TYPE_B was acknowledged, which freed its slot; the unit test
        // test_sync_request_skips_a_withdrawal_still_being_resent covers a pending one.
        const auto notification = CdmpStateChangeMessage::FromCanFrame(frame->data);
        zassert_equal(notification.state_type, state_type, "In type order");
        replay_sequences.push_back(frame->sequence);
    }

    auto response = harness.bus.Wait(COMMAND_RESPONSE, IsFrom(OWN_ID));
    zassert_true(response.has_value(), "A unicast request is answered");
    const auto message = CdmpCommandResponseMessage::FromCanFrame(response->data);
    zassert_equal(message.command_code, std::to_underlying(CdmpServiceCommandCode::STATE_SYNC_REQUEST));
    zassert_equal(message.transaction_id, 0x42);
    zassert_equal(message.result_code, CdmpResultCode::SUCCESS);
    zassert_equal(message.data.size(), 1);
    zassert_equal(message.data[0], 2, "With the record count");
    zassert_true(response->sequence > replay_sequences.back(), "Once the records are queued");

    zassert_false(harness.WaitForNotification(NO_FRAME_TIMEOUT_MS).has_value(), "A replay is not resent");
}

ZTEST_F(cdmp_state_sync, test_broadcast_sync_request_replays_records_without_answer) {
    Harness& harness = *fixture->harness;
    zassert_true(harness.State()->SetLocalState(TYPE_A, Bytes{ 0x01 }));

    harness.BringOnline();
    auto announcement = harness.WaitForNotification();
    zassert_true(announcement.has_value());
    harness.SendAsPeer(STATE_CHANGE_RESPONSE, AcknowledgementFrame(PEER_ID, OWN_ID, announcement->version));
    harness.Settle();
    harness.bus.Clear();

    harness.SendAsPeer(COMMAND, SyncRequestFrame(CdmpDevice::DEVICE_ID_BROADCAST, 0x43));

    auto notification = harness.WaitForNotification();
    zassert_true(notification.has_value(), "The unit replays its records to a broadcast request");
    zassert_equal(notification->state_type, TYPE_A);
    zassert_equal(notification->version, 1);

    zassert_false(
        harness.bus.Wait(COMMAND_RESPONSE, IsFrom(OWN_ID), NO_FRAME_TIMEOUT_MS).has_value(),
        "A broadcast request gets no response"
    );
}

ZTEST_F(cdmp_state_sync, test_peer_record_is_acknowledged_and_applied) {
    Harness& harness = *fixture->harness;
    harness.BringOnline();
    harness.Settle();

    harness.SendAsPeer(STATE_CHANGE, NotificationFrame(PEER_ID, 7, TYPE_A, { 0x05 }));

    auto frame = harness.bus.Wait(STATE_CHANGE_RESPONSE, IsFrom(OWN_ID));
    zassert_true(frame.has_value(), "The unit acknowledges the peer's notification");
    const auto acknowledgement = CdmpStateChangeResponseMessage::FromCanFrame(frame->data);
    zassert_equal(acknowledgement.source_device_id, PEER_ID);
    zassert_equal(acknowledgement.version, 7);

    std::array<CdmpRemoteState, 4> states{};
    zassert_equal(harness.State()->GetRemoteStates(TYPE_A, states), 1);
    zassert_equal(states[0].device_id, PEER_ID);
    zassert_true(states[0].data == CdmpStateData(Bytes{ 0x05 }));
}

ZTEST_F(cdmp_state_sync, test_own_frames_are_ignored) {
    Harness& harness = *fixture->harness;
    harness.BringOnline();
    harness.Settle();

    // The loopback controller hands the unit its own notification and each of its resends. Both the
    // own-ID check and the device table, which never holds the unit's own ID, reject them; this
    // covers the rule on a real controller, not one of the two checks.
    zassert_true(harness.State()->SetLocalState(TYPE_A, Bytes{ 0x01 }));
    zassert_true(harness.WaitForNotification().has_value());
    zassert_true(harness.WaitForNotification().has_value());

    zassert_false(
        harness.bus.Wait(STATE_CHANGE_RESPONSE, IsFrom(OWN_ID), NO_FRAME_TIMEOUT_MS).has_value(),
        "The unit does not acknowledge its own notifications"
    );

    std::array<CdmpRemoteState, 4> states{};
    zassert_equal(harness.State()->GetRemoteStates(TYPE_A, states), 0, "Nor keep a copy of them");
}

ZTEST_F(cdmp_state_sync, test_records_survive_a_restart) {
    Harness& harness = *fixture->harness;
    zassert_true(harness.State()->SetLocalState(TYPE_A, Bytes{ 0x01 }));
    harness.BringOnline();
    zassert_true(harness.WaitForNotification().has_value());
    harness.SendAsPeer(STATE_CHANGE_RESPONSE, AcknowledgementFrame(PEER_ID, OWN_ID, 1));
    harness.Settle();

    zassert_true(harness.unit->Stop());
    // Set while stopped: announced on the next ONLINE.
    zassert_true(harness.State()->SetLocalState(TYPE_B, Bytes{ 0x02 }));
    harness.bus.Clear();

    harness.BringOnline();

    for(uint8_t state_type : { TYPE_A, TYPE_B }) {
        auto notification = harness.WaitForNotification();
        zassert_true(notification.has_value(), "The records outlive the stop");
        zassert_equal(notification->state_type, state_type);
        zassert_equal(notification->version, state_type == TYPE_A ? 1 : 2);
    }
}
