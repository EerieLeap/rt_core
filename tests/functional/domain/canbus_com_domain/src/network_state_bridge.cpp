#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "canbus_com_test_support.h"
#include "network_state_test_support.h"

using namespace network_state_test;

namespace {

constexpr uint8_t LOGGING_STATUS_CODE = std::to_underlying(CanbusComStateType::LOGGING_STATUS);

// The records as other units send them: flags (bit 0 active), then the reason while inactive.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization): test fixture built before main; a failed allocation aborts the run either way
const Bytes ACTIVE = { 0x01, 0x00 };
// NOLINTNEXTLINE(bugprone-throwing-static-initialization): test fixture built before main; a failed allocation aborts the run either way
const Bytes STOPPED = { 0x00, 0x00 };
// NOLINTNEXTLINE(bugprone-throwing-static-initialization): test fixture built before main; a failed allocation aborts the run either way
const Bytes WRITE_FAILED = { 0x00, 0x01 };
// NOLINTNEXTLINE(bugprone-throwing-static-initialization): test fixture built before main; a failed allocation aborts the run either way
const Bytes NO_SPACE = { 0x00, 0x02 };

constexpr uint32_t ReasonValue(LoggingStatusReason reason) {
    return static_cast<uint32_t>(reason);
}

void AssertSetLocalState(FakeTransport& transport, const Bytes& expected, bool is_accepted, const char* message) {
    auto change = transport.WaitForLocalChange();
    zassert_true(change.has_value(), "%s: the record changes", message);
    zassert_equal(change->state_type, CanbusComStateType::LOGGING_STATUS, "%s: as LOGGING_STATUS", message);
    zassert_true(change->data == Data(expected), "%s: to the expected bytes", message);
    zassert_equal(
        change->is_accepted, is_accepted, "%s: %s by the transport", message, is_accepted ? "taken" : "refused"
    );
}

void AssertLocalChange(FakeTransport& transport, const Bytes& expected, const char* message) {
    AssertSetLocalState(transport, expected, true, message);
}

void AssertRefusedChange(FakeTransport& transport, const Bytes& expected, const char* message) {
    AssertSetLocalState(transport, expected, false, message);
}

void AssertImported(
    StatusProbe& probe,
    bool is_active,
    std::optional<LoggingStatusReason> reason,
    const char* message
) {
    auto event = probe.WaitForEvent();
    zassert_true(event.has_value(), "%s: the importer publishes", message);
    zassert_equal(event->source_id, IMPORTED_EVENT_SOURCE_ID, "%s: as an imported event", message);
    zassert_true(event->is_active.has_value() && *event->is_active == is_active, "%s: IsActive", message);

    if(reason.has_value()) {
        zassert_true(event->reason.has_value(), "%s: with a reason", message);
        zassert_equal(*event->reason, ReasonValue(*reason), "%s: the expected reason", message);
    } else {
        zassert_false(event->reason.has_value(), "%s: without a reason", message);
    }
}

/** @brief A channel with the events of logging_events, which no bus carries until a test registers it. */
class LateLoggingEventsChannel : public LoggingChannel {
private:
    LateLoggingEventsChannel() :
        LoggingChannel("late_logging_events", LAST_LOGGING_EVENT_TYPE, LAST_LOGGING_PAYLOAD_TYPE, 8) {}

public:
    static LateLoggingEventsChannel& GetInstance() {
        static LateLoggingEventsChannel channel;

        return channel;
    }
};

/** @brief The logging status on LateLoggingEventsChannel. */
struct LateLoggingNetworkState : LoggingNetworkState {
    using Channel = LateLoggingEventsChannel;
};

using LateLoggingImporter = NetworkStateImporter<LateLoggingNetworkState>;

} // namespace

// ======== With a fake transport ========

struct network_state_bridge_fixture {
    std::shared_ptr<FakeTransport> transport;
    std::unique_ptr<StatusProbe> probe;
    std::unique_ptr<LoggingExporter> exporter;
    std::unique_ptr<LoggingImporter> importer;
    std::unique_ptr<LateLoggingImporter> late_importer;
};

static void* NetworkStateBridgeSetup() {
    static network_state_bridge_fixture fixture{};

    BridgeTestBus::GetInstance();

    return &fixture;
}

static void NetworkStateBridgeBefore(void* data) {
    auto* fixture = static_cast<network_state_bridge_fixture*>(data);

    fixture->transport = std::make_shared<FakeTransport>();
    fixture->probe = std::make_unique<StatusProbe>();
    // Events of the previous test, which a failure may have left queued, do not reach this one.
    fixture->probe->Flush();
}

static void NetworkStateBridgeAfter(void* data) {
    auto* fixture = static_cast<network_state_bridge_fixture*>(data);

    fixture->exporter.reset();
    fixture->importer.reset();
    fixture->late_importer.reset();
    fixture->probe.reset();
    fixture->transport.reset();
}

ZTEST_SUITE(
    network_state_bridge,
    NULL,
    NetworkStateBridgeSetup,
    NetworkStateBridgeBefore,
    NetworkStateBridgeAfter,
    NULL
);

ZTEST(network_state_bridge, test_imported_events_have_a_source_of_their_own) {
    // The log writer publishes with source 0, and every other publisher with the hash of its function.
    zassert_not_equal(IMPORTED_EVENT_SOURCE_ID, 0);
    zassert_not_equal(IMPORTED_EVENT_SOURCE_ID, FENCE_SOURCE_ID);
}

ZTEST_F(network_state_bridge, test_exporter_shares_the_initial_state) {
    FakeTransport& transport = *fixture->transport;

    fixture->exporter = std::make_unique<LoggingExporter>(fixture->transport);

    AssertLocalChange(transport, STOPPED, "Before any event, the unit shares Initial(): inactive, Stopped");
    zassert_false(transport.WaitForLocalChange(NO_EVENT_TIMEOUT_MS).has_value(), "and nothing else");
}

ZTEST_F(network_state_bridge, test_exporter_maps_status_events) {
    FakeTransport& transport = *fixture->transport;
    fixture->exporter = std::make_unique<LoggingExporter>(fixture->transport);
    AssertLocalChange(transport, STOPPED, "Initial");

    PublishStatus(true);
    AssertLocalChange(transport, ACTIVE, "A start");

    // Each check below waits for the record of the event after the one it checks: the channel
    // delivers in order, so a change for the checked event would have come first.
    PublishStatus(true);
    PublishStatus(false, LoggingStatusReason::WriteFailed);
    AssertLocalChange(transport, WRITE_FAILED, "A repeated start changes nothing, a failure does");

    PublishStatus(false, LoggingStatusReason::Stopped);
    PublishStatus(true);
    AssertLocalChange(transport, ACTIVE, "The cleanup stop keeps the failure, and the next start clears it");

    LoggingEventsChannel::GetInstance().PublishAsync({ .type = LoggingEventType::StatusUpdated });
    PublishStatus(false, LoggingStatusReason::NoSpace);
    AssertLocalChange(transport, NO_SPACE, "An event without IsActive changes nothing");

    PublishStatus(true);
    AssertLocalChange(transport, ACTIVE, "Another start");
    PublishStatus(false);
    AssertLocalChange(transport, STOPPED, "A stop without a failure, with the reason left out, is Stopped");

    zassert_false(transport.WaitForLocalChange(NO_EVENT_TIMEOUT_MS).has_value());
}

ZTEST_F(network_state_bridge, test_exporter_skips_imported_events) {
    FakeTransport& transport = *fixture->transport;
    fixture->exporter = std::make_unique<LoggingExporter>(fixture->transport);
    AssertLocalChange(transport, STOPPED, "Initial");

    // What an importer of another unit's status would publish here.
    PublishStatus(true, std::nullopt, IMPORTED_EVENT_SOURCE_ID);
    PublishStatus(false, LoggingStatusReason::WriteFailed);
    AssertLocalChange(transport, WRITE_FAILED, "The imported start never becomes this unit's record");

    PublishStatus(false, LoggingStatusReason::NoSpace, IMPORTED_EVENT_SOURCE_ID);
    zassert_false(transport.WaitForLocalChange(NO_EVENT_TIMEOUT_MS).has_value(), "Nor does an imported failure");
}

ZTEST_F(network_state_bridge, test_exporter_withdraws_its_record_on_destruction) {
    FakeTransport& transport = *fixture->transport;
    fixture->exporter = std::make_unique<LoggingExporter>(fixture->transport);
    AssertLocalChange(transport, STOPPED, "Initial");

    PublishStatus(true);
    AssertLocalChange(transport, ACTIVE, "A start");

    fixture->exporter.reset();

    auto withdrawal = transport.WaitForLocalChange();
    zassert_true(withdrawal.has_value(), "Destruction withdraws the record");
    zassert_equal(withdrawal->state_type, CanbusComStateType::LOGGING_STATUS);
    zassert_true(withdrawal->data.IsEmpty(), "with empty data");

    PublishStatus(false, LoggingStatusReason::WriteFailed);
    zassert_false(transport.WaitForLocalChange(NO_EVENT_TIMEOUT_MS).has_value(), "and later events no longer reach it");
}

ZTEST_F(network_state_bridge, test_exporter_offers_a_refused_record_again) {
    FakeTransport& transport = *fixture->transport;
    // As when every record slot of the unit holds another state type.
    transport.RefuseLocalStates(true);
    fixture->exporter = std::make_unique<LoggingExporter>(fixture->transport);
    AssertRefusedChange(transport, STOPPED, "Initial");

    transport.RefuseLocalStates(false);
    PublishStatus(false);
    AssertLocalChange(transport, STOPPED, "The next event offers the initial record again, although it maps to it");

    transport.RefuseLocalStates(true);
    PublishStatus(true);
    AssertRefusedChange(transport, ACTIVE, "A start");
    PublishStatus(false, LoggingStatusReason::WriteFailed);
    AssertRefusedChange(transport, WRITE_FAILED, "A failure");

    transport.RefuseLocalStates(false);
    PublishStatus(false, LoggingStatusReason::Stopped);
    AssertLocalChange(
        transport, WRITE_FAILED,
        "The cleanup stop offers the refused failure again: the record follows the events, not the transport"
    );

    PublishStatus(false, LoggingStatusReason::Stopped);
    zassert_false(
        transport.WaitForLocalChange(NO_EVENT_TIMEOUT_MS).has_value(),
        "Once the transport took the record, an event that keeps it offers nothing"
    );
}

ZTEST_F(network_state_bridge, test_importer_catches_up_on_registration) {
    FakeTransport& transport = *fixture->transport;
    StatusProbe& probe = *fixture->probe;
    // Two units report before the importer exists, and the one with the higher ID logs.
    transport.SetRemoteState(1, WRITE_FAILED);
    transport.SetRemoteState(4, ACTIVE);

    fixture->importer = std::make_unique<LoggingImporter>(fixture->transport);

    AssertImported(probe, true, std::nullopt, "The first call combines the records already there");
    zassert_false(probe.WaitForEvent(NO_EVENT_TIMEOUT_MS).has_value(), "once");
    zassert_equal(transport.GetHandlerCount(), 1);
}

ZTEST_F(network_state_bridge, test_importer_without_records_publishes_unavailable) {
    fixture->importer = std::make_unique<LoggingImporter>(fixture->transport);

    AssertImported(*fixture->probe, false, LoggingStatusReason::Unavailable, "No unit reports a status");
}

ZTEST_F(network_state_bridge, test_importer_publishes_the_combined_state_once_per_change) {
    FakeTransport& transport = *fixture->transport;
    StatusProbe& probe = *fixture->probe;
    fixture->importer = std::make_unique<LoggingImporter>(fixture->transport);
    AssertImported(probe, false, LoggingStatusReason::Unavailable, "First call");

    transport.SetRemoteState(1, STOPPED);
    transport.NotifyHandlers();
    AssertImported(probe, false, LoggingStatusReason::Stopped, "A unit that does not log");

    transport.NotifyHandlers();
    zassert_false(probe.WaitForEvent(NO_EVENT_TIMEOUT_MS).has_value(), "A call without a change publishes nothing");

    transport.SetRemoteState(5, WRITE_FAILED);
    transport.NotifyHandlers();
    AssertImported(probe, false, LoggingStatusReason::WriteFailed, "A failure is preferred over Stopped");

    transport.SetRemoteState(3, NO_SPACE);
    transport.NotifyHandlers();
    AssertImported(probe, false, LoggingStatusReason::NoSpace, "The failure of the lowest device ID wins");

    transport.SetRemoteState(5, ACTIVE);
    transport.NotifyHandlers();
    AssertImported(probe, true, std::nullopt, "Any unit that logs makes it active");

    transport.SetRemoteState(1, ACTIVE);
    transport.SetRemoteState(3, STOPPED);
    transport.NotifyHandlers();
    zassert_false(
        probe.WaitForEvent(NO_EVENT_TIMEOUT_MS).has_value(),
        "Changed records that combine to the same state publish nothing"
    );

    transport.RemoveRemoteState(1);
    transport.RemoveRemoteState(3);
    transport.RemoveRemoteState(5);
    transport.NotifyHandlers();
    AssertImported(probe, false, LoggingStatusReason::Unavailable, "Once every record is gone");
}

ZTEST_F(network_state_bridge, test_importer_ignores_other_state_types) {
    FakeTransport& transport = *fixture->transport;
    StatusProbe& probe = *fixture->probe;
    fixture->importer = std::make_unique<LoggingImporter>(fixture->transport);
    AssertImported(probe, false, LoggingStatusReason::Unavailable, "First call");

    constexpr auto OTHER_TYPE = static_cast<CanbusComStateType>(0x7F);
    transport.SetRemoteState(1, ACTIVE, OTHER_TYPE);
    transport.NotifyHandlers(OTHER_TYPE);
    transport.NotifyHandlers();
    zassert_false(probe.WaitForEvent(NO_EVENT_TIMEOUT_MS).has_value(), "Another type's records are not combined");
}

ZTEST_F(network_state_bridge, test_importer_unregisters_on_destruction) {
    FakeTransport& transport = *fixture->transport;
    StatusProbe& probe = *fixture->probe;
    fixture->importer = std::make_unique<LoggingImporter>(fixture->transport);
    AssertImported(probe, false, LoggingStatusReason::Unavailable, "First call");

    fixture->importer.reset();
    zassert_equal(transport.GetHandlerCount(), 0, "Destruction unregisters the handler");

    transport.SetRemoteState(1, ACTIVE);
    transport.NotifyHandlers();
    zassert_false(probe.WaitForEvent(NO_EVENT_TIMEOUT_MS).has_value(), "and nothing is published any more");
}

ZTEST_F(network_state_bridge, test_importer_publishes_once_a_bus_carries_the_channel) {
    FakeTransport& transport = *fixture->transport;
    LateLoggingEventsChannel& channel = LateLoggingEventsChannel::GetInstance();
    zassert_false(channel.IsRegistered(), "No bus carries the channel yet");

    // The first call combines the record at once, and the inert channel would drop the event.
    transport.SetRemoteState(1, ACTIVE);
    fixture->late_importer = std::make_unique<LateLoggingImporter>(fixture->transport);

    zassert_ok(BridgeTestBus::GetInstance().RegisterChannel(channel));
    StatusProbe probe(channel);

    transport.NotifyHandlers();
    AssertImported(probe, true, std::nullopt, "The next call publishes the state, although it did not change");

    transport.NotifyHandlers();
    zassert_false(probe.WaitForEvent(NO_EVENT_TIMEOUT_MS).has_value(), "and then only a change publishes");
}

// ======== Through CDMP ========

namespace {

using canbus_com_test::Harness;
using cdmp_loopback_test::AcknowledgementFrame;
using cdmp_loopback_test::NotificationFrame;
using cdmp_loopback_test::OWN_ID;
using cdmp_loopback_test::PEER_ID;
using cdmp_loopback_test::STATE_CHANGE;
using cdmp_loopback_test::STATE_CHANGE_RESPONSE;

} // namespace

struct network_state_bridge_cdmp_fixture {
    Harness* harness;
    std::unique_ptr<StatusProbe> probe;
    std::unique_ptr<LoggingExporter> exporter;
    std::unique_ptr<LoggingImporter> importer;
};

static void* NetworkStateBridgeCdmpSetup() {
    static network_state_bridge_cdmp_fixture fixture{};

    BridgeTestBus::GetInstance();

    return &fixture;
}

static void NetworkStateBridgeCdmpBefore(void* data) {
    auto* fixture = static_cast<network_state_bridge_cdmp_fixture*>(data);

    fixture->probe = std::make_unique<StatusProbe>();
    fixture->probe->Flush();
    fixture->harness = new Harness();
}

static void NetworkStateBridgeCdmpAfter(void* data) {
    auto* fixture = static_cast<network_state_bridge_cdmp_fixture*>(data);

    // Before the harness: both hold the COM service.
    fixture->exporter.reset();
    fixture->importer.reset();

    delete fixture->harness;
    fixture->harness = nullptr;
    fixture->probe.reset();

    cdmp_loopback_test::ResetLoopbackDevice();
}

ZTEST_SUITE(
    network_state_bridge_cdmp,
    NULL,
    NetworkStateBridgeCdmpSetup,
    NetworkStateBridgeCdmpBefore,
    NetworkStateBridgeCdmpAfter,
    NULL
);

ZTEST_F(network_state_bridge_cdmp, test_exporter_shares_the_status_with_other_units) {
    Harness& harness = *fixture->harness;
    // Created before the start, as a controller's Initialize() does: the record waits for ONLINE.
    fixture->exporter = std::make_unique<LoggingExporter>(harness.com);

    harness.BringOnline();

    auto initial = harness.WaitForNotification();
    zassert_true(initial.has_value(), "The unit announces its status on going online");
    zassert_equal(initial->state_type, LOGGING_STATUS_CODE);
    zassert_true(initial->data == Data(STOPPED), "Initial(): inactive, Stopped");
    harness.SendAsPeer(STATE_CHANGE_RESPONSE, AcknowledgementFrame(PEER_ID, OWN_ID, initial->version));
    // The unit also replays the record to its own broadcast sync request, after its stagger. Dropped
    // here, so that the notifications below are the start's and the withdrawal's whenever it runs.
    harness.Settle();
    harness.bus.Clear();

    PublishStatus(true);

    auto started = harness.WaitForNotification();
    zassert_true(started.has_value(), "A start goes out to the other units");
    zassert_true(started->data == Data(ACTIVE));
    zassert_equal(started->version, initial->version + 1);
    harness.SendAsPeer(STATE_CHANGE_RESPONSE, AcknowledgementFrame(PEER_ID, OWN_ID, started->version));

    fixture->exporter.reset();

    auto withdrawal = harness.WaitForNotification();
    zassert_true(withdrawal.has_value(), "Destroying the exporter withdraws the record");
    zassert_equal(withdrawal->state_type, LOGGING_STATUS_CODE);
    zassert_true(withdrawal->data.IsEmpty());
}

ZTEST_F(network_state_bridge_cdmp, test_importer_publishes_the_status_of_other_units) {
    Harness& harness = *fixture->harness;
    StatusProbe& probe = *fixture->probe;
    fixture->importer = std::make_unique<LoggingImporter>(harness.com);

    AssertImported(probe, false, LoggingStatusReason::Unavailable, "Not online yet, no unit reports a status");

    harness.BringOnline();
    harness.Settle();

    harness.SendAsPeer(STATE_CHANGE, NotificationFrame(PEER_ID, 1, LOGGING_STATUS_CODE, WRITE_FAILED));
    AssertImported(probe, false, LoggingStatusReason::WriteFailed, "Another unit's failure");

    harness.SendAsPeer(STATE_CHANGE, NotificationFrame(PEER_ID, 2, LOGGING_STATUS_CODE, ACTIVE));
    AssertImported(probe, true, std::nullopt, "Its start");

    // As on every CAN reconfiguration: the unit leaves ONLINE and drops the other units' records.
    zassert_true(harness.com->Stop());
    AssertImported(probe, false, LoggingStatusReason::Unavailable, "Offline, no unit reports a status");
}
