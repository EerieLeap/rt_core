#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

#include <zephyr/ztest.h>

#include "domain/canbus_com_domain/commands/canbus_com_command_code.h"
#include "domain/canbus_com_domain/state_bridge/network_state_importer.h"
#include "domain/canbus_com_domain/states/canbus_com_state_type.h"
#include "domain/logging_domain/control/canbus_com_logging_command_sender.h"
#include "domain/logging_domain/control/remote_logging_control.h"
#include "domain/logging_domain/event_bus/logging_network_state.h"

#include "canbus_com_loopback_harness.h"
#include "logging_control_test_support.h"

using namespace logging_control_test;
using namespace canbus_com_loopback_test;

using eerie_leap::domain::canbus_com_domain::commands::CanbusComCommandCode;
using eerie_leap::domain::canbus_com_domain::state_bridge::NetworkStateImporter;
using eerie_leap::domain::canbus_com_domain::states::CanbusComStateType;
using eerie_leap::domain::logging_domain::control::CanbusComLoggingCommandSender;
using eerie_leap::domain::logging_domain::control::RemoteLoggingControl;
using eerie_leap::domain::logging_domain::event_bus::LoggingNetworkState;

namespace {

constexpr uint8_t LOGGING_CODE = std::to_underlying(CanbusComCommandCode::LOGGING);
constexpr uint8_t LOGGING_STATUS_CODE = std::to_underlying(CanbusComStateType::LOGGING_STATUS);
constexpr uint8_t BROADCAST = CdmpDevice::DEVICE_ID_BROADCAST;

using LoggingImporter = NetworkStateImporter<LoggingNetworkState>;

// A LOGGING command on the bus: the broadcast, the code, the transaction, then start or stop.
bool IsLoggingBroadcast(const Bytes& frame) {
    return frame.size() == 4 && frame[0] == BROADCAST && frame[1] == LOGGING_CODE;
}

// Records the StatusUpdated events in order, as a consumer of logging_events sees them.
class StatusLog {
public:
    struct Entry {
        bool is_active;
        std::optional<LoggingStatusReason> reason;
    };

private:
    struct Inbox {
        k_sem received;
        k_mutex lock;
        std::deque<Entry> entries;

        Inbox() {
            k_sem_init(&received, 0, K_SEM_MAX_LIMIT);
            k_mutex_init(&lock);
        }
    };

    std::shared_ptr<Inbox> inbox_ = std::make_shared<Inbox>();
    AnySubscription subscription_;

public:
    StatusLog() {
        subscription_ = CreateScopedSubscription(
            LoggingEventsChannel::GetInstance(),
            LoggingEventType::StatusUpdated,
            [inbox = inbox_](const LoggingEventsChannel::EventMessage& event) {
                const auto active = event.payload.find(LoggingPayloadType::IsActive);
                if(active == event.payload.end() || !std::holds_alternative<bool>(active->second))
                    return;

                Entry entry { .is_active = std::get<bool>(active->second) };
                const auto reason = event.payload.find(LoggingPayloadType::Reason);
                if(reason != event.payload.end() && std::holds_alternative<uint32_t>(reason->second))
                    entry.reason = static_cast<LoggingStatusReason>(std::get<uint32_t>(reason->second));

                {
                    ScopedMutex guard(inbox->lock);
                    inbox->entries.push_back(entry);
                }
                k_sem_give(&inbox->received);
            });
        zassert_not_null(subscription_.get(), "The status log's subscription was refused.");
    }

    std::optional<Entry> WaitForEntry(int timeout_ms = k_wait_ms) {
        if(k_sem_take(&inbox_->received, Ms(timeout_ms)) != 0)
            return std::nullopt;

        ScopedMutex guard(inbox_->lock);
        auto entry = inbox_->entries.front();
        inbox_->entries.pop_front();

        return entry;
    }
};

// The unit and a scripted peer on its bus, brought online.
struct Fixture {
    Harness harness;
    uint8_t next_peer_version = 1;

    Fixture() {
        harness.BringOnline();
        harness.Settle();
    }

    // The peer's status record, as a logging unit shares it.
    void ShareStatus(const Bytes& data) {
        harness.SendAsPeer(STATE_CHANGE, NotificationFrame(PEER_ID, next_peer_version, LOGGING_STATUS_CODE, data));
        zassert_true(harness.WaitForAcknowledgement(PEER_ID, next_peer_version), "The unit applied the record");
        next_peer_version++;
    }

    // The next LOGGING broadcast of the unit, or nothing in time.
    std::optional<RecordedFrame> WaitForBroadcast(int timeout_ms = k_wait_ms) {
        return harness.bus.Wait(COMMAND, IsLoggingBroadcast, timeout_ms);
    }
};

Fixture*& Current() {
    static Fixture* fixture = nullptr;

    return fixture;
}

void* SetUp() {
    return SetUpChannels();
}

void Before(void*) {
    Current() = new Fixture();
}

void After(void*) {
    Gate().Open();

    delete Current();
    Current() = nullptr;

    ResetLoopbackDevice();
}

Fixture& F() {
    return *Current();
}

} // namespace

ZTEST_SUITE(canbus_com_logging_command_sender, NULL, SetUp, Before, After, NULL);

ZTEST(canbus_com_logging_command_sender, test_TheCommandIsTheLoggingBroadcast) {
    CanbusComLoggingCommandSender sender(F().harness.com);

    sender.SendLoggingCommand(true);
    const auto start = F().WaitForBroadcast();
    zassert_true(start.has_value(), "A start must go out on the command identifier");
    zassert_equal(start->data[3], 0x01, "start");

    sender.SendLoggingCommand(false);
    const auto stop = F().WaitForBroadcast();
    zassert_true(stop.has_value());
    zassert_equal(stop->data[3], 0x00, "stop");
    zassert_not_equal(stop->data[2], start->data[2], "Each command is a transaction of its own");
}

ZTEST(canbus_com_logging_command_sender, test_AResponseAfterTheSenderIsGoneIsHarmless) {
    auto sender = std::make_unique<CanbusComLoggingCommandSender>(F().harness.com);

    sender->SendLoggingCommand(true);
    const auto sent = F().WaitForBroadcast();
    zassert_true(sent.has_value());

    sender.reset();

    // A late answer completes the transaction and runs the callback, which holds only the flag.
    F().harness.SendAsPeer(COMMAND_RESPONSE, CdmpCommandResponseMessage{
        .source_device_id = PEER_ID,
        .command_code = LOGGING_CODE,
        .transaction_id = sent->data[2],
        .result_code = CdmpResultCode::FAILURE,
        .data = {}}.ToCanFrame());
    k_msleep(NO_FRAME_TIMEOUT_MS);

    CanbusComLoggingCommandSender later(F().harness.com);
    later.SendLoggingCommand(false);
    zassert_true(F().WaitForBroadcast().has_value(), "A new sender still works");
}

ZTEST(canbus_com_logging_command_sender, test_ANullServiceIsRefused) {
    bool is_refused = false;
    try {
        CanbusComLoggingCommandSender sender(nullptr);
    } catch(const std::invalid_argument&) {
        is_refused = true;
    }

    zassert_true(is_refused, "A sender without the COM service could send nothing");
}

// L2's premise on the real path: the importer's Unavailable, then the Stopped a logging unit shares.
ZTEST(canbus_com_logging_command_sender, test_AStartNoUnitReceivedIsSentAgainOnceALoggingUnitReports) {
    // In the order the controller uses: the control, then the importer, so its first status is seen.
    RemoteLoggingControl control(TestConfigQueue(), std::make_shared<CanbusComLoggingCommandSender>(F().harness.com));
    zassert_true(control.Initialize());

    StatusLog log;
    auto importer = std::make_unique<LoggingImporter>(F().harness.com);

    auto status = log.WaitForEntry();
    zassert_true(status.has_value(), "The importer says where it stands");
    zassert_false(status->is_active);
    zassert_equal(status->reason, LoggingStatusReason::Unavailable, "No unit shares a status");

    control.RequestStart();
    zassert_true(F().WaitForBroadcast().has_value(), "The start goes out");
    zassert_false(F().WaitForBroadcast(NO_FRAME_TIMEOUT_MS).has_value(), "once");

    // A logging unit appears, idle.
    F().ShareStatus({0x00, 0x00});
    status = log.WaitForEntry();
    zassert_true(status.has_value());
    zassert_equal(status->reason, LoggingStatusReason::Stopped, "A unit reports it is idle");
    const auto again = F().WaitForBroadcast();
    zassert_true(again.has_value(), "so the start that nobody received is sent again");
    zassert_equal(again->data[3], 0x01);

    // It logs, then stops: that is its own doing.
    F().ShareStatus({0x01});
    F().ShareStatus({0x00, 0x00});
    zassert_false(F().WaitForBroadcast(NO_FRAME_TIMEOUT_MS).has_value(), "and nothing is sent a third time");

    importer.reset();
}

// The limit of that premise. A logging unit that reboots within the heartbeat timeout claims its ID
// again with the same UID: CDMP reports RESTARTED, not LEFT, so its records turn stale but stay,
// and the status never passes through Unavailable. What happens next depends on what the unit's
// first heartbeat says about its records:
//
// - It already holds its status record (the exporter was built before it went online), so the
//   heartbeat carries the digest of that record. The stale copy differs, nothing is withdrawn, and
//   the record that follows replaces it: the status goes from logging straight to Stopped, which the
//   rule does not re-assert. A start the reboot cost stays lost.
// - It holds no record yet (digest 0), so the heartbeat withdraws the stale copy: Unavailable, then
//   Stopped when the record arrives, which re-asserts.
//
// Both are pinned, because the first is a known limit of L2 and the second is why it works at all.
//
// The first case contradicts the plan's L2 sentence "A DAQ reboot therefore clears a shell stop while
// the gauge's switch is on" and its phase 6 bench row "DAQ rebooted while the gauge's switch is on:
// logging resumes". It pins what the rule does today, not what the plan wants, and only until the
// open decision on how a restart is seen is made: (a) CDMP drops a restarted source's records, (b)
// the transport reports "a source restarted or joined" and the control re-asserts on the first
// inactive Stopped after it, or (c) the plan is amended. Invert the assertion if (a) or (b) is chosen.

// The peer claims its ID again and sends its first heartbeat, with the digest of @p records.
static void RestartPeer(Fixture& fixture, std::initializer_list<Record> records) {
    fixture.harness.SendAsPeer(MANAGEMENT, CdmpIdClaimRequestMessage{
        .claiming_device_id = PEER_ID,
        .uid = PEER_UID,
        .device_type = CdmpDeviceType::LOGGER,
        .protocol_version = CdmpDevice::GetCurrentProtocolVersion()}.ToCanFrame());
    fixture.harness.SendAsPeer(HEARTBEAT, HeartbeatFrame(PEER_ID, Digest(records)));
}

ZTEST(canbus_com_logging_command_sender, test_KnownLimitAQuickRestartThatKeepsItsRecordIsNotReasserted) {
    RemoteLoggingControl control(TestConfigQueue(), std::make_shared<CanbusComLoggingCommandSender>(F().harness.com));
    zassert_true(control.Initialize());

    StatusLog log;
    auto importer = std::make_unique<LoggingImporter>(F().harness.com);
    zassert_true(log.WaitForEntry().has_value(), "Unavailable");

    control.RequestStart();
    zassert_true(F().WaitForBroadcast().has_value(), "The start goes out");

    F().ShareStatus({0x01});
    auto status = log.WaitForEntry();
    zassert_true(status.has_value());
    zassert_true(status->is_active, "The unit logs");

    // The reboot; the unit's new record is version 1 again, and its heartbeat already covers it.
    RestartPeer(F(), {{LOGGING_STATUS_CODE, 1, {0x00, 0x00}}});
    zassert_true(F().harness.bus.Wait(COMMAND, IsSyncRequestTo(PEER_ID)).has_value(),
        "The unit asks the restarted unit to replay its records, as its digest differs");
    // A real unit announces its record long after it claimed its ID, and a version it already held
    // is a duplicate only until the claim has been processed.
    k_msleep(NO_FRAME_TIMEOUT_MS);

    F().next_peer_version = 1;
    F().ShareStatus({0x00, 0x00});

    status = log.WaitForEntry();
    zassert_true(status.has_value());
    zassert_false(status->is_active);
    zassert_equal(status->reason, LoggingStatusReason::Stopped, "Stopped follows the active status directly");

    zassert_false(F().WaitForBroadcast(NO_FRAME_TIMEOUT_MS).has_value(),
        "No Unavailable came between, so nothing is re-asserted");

    importer.reset();
}

ZTEST(canbus_com_logging_command_sender, test_AQuickRestartThatAnnouncesItsRecordLaterIsReasserted) {
    RemoteLoggingControl control(TestConfigQueue(), std::make_shared<CanbusComLoggingCommandSender>(F().harness.com));
    zassert_true(control.Initialize());

    StatusLog log;
    auto importer = std::make_unique<LoggingImporter>(F().harness.com);
    zassert_true(log.WaitForEntry().has_value(), "Unavailable");

    control.RequestStart();
    zassert_true(F().WaitForBroadcast().has_value(), "The start goes out");

    F().ShareStatus({0x01});
    auto status = log.WaitForEntry();
    zassert_true(status.has_value());
    zassert_true(status->is_active, "The unit logs");

    // The reboot, and a first heartbeat that says the unit holds no records yet.
    RestartPeer(F(), {});
    status = log.WaitForEntry();
    zassert_true(status.has_value());
    zassert_false(status->is_active);
    zassert_equal(status->reason, LoggingStatusReason::Unavailable, "The stale copy is withdrawn");

    F().next_peer_version = 1;
    F().ShareStatus({0x00, 0x00});
    status = log.WaitForEntry();
    zassert_true(status.has_value());
    zassert_equal(status->reason, LoggingStatusReason::Stopped);

    zassert_true(F().WaitForBroadcast().has_value(), "Unavailable, then Stopped: the start is sent again");

    importer.reset();
}

// The reverse of the limit above, from the plan's risks: this unit's own COM service restarts (a CAN
// reconfiguration takes CDMP out of ONLINE), which makes it forget every other unit's records. The
// status goes to Unavailable and, when the logging unit announces itself again, to Stopped, so a
// wanted start is sent again even if a stop on the logging unit's shell had ended the session.
ZTEST(canbus_com_logging_command_sender, test_ARestartOfTheUnitsOwnComServiceReassertsAStopFromTheShell) {
    RemoteLoggingControl control(TestConfigQueue(), std::make_shared<CanbusComLoggingCommandSender>(F().harness.com));
    zassert_true(control.Initialize());

    StatusLog log;
    auto importer = std::make_unique<LoggingImporter>(F().harness.com);
    zassert_true(log.WaitForEntry().has_value(), "Unavailable");

    control.RequestStart();
    zassert_true(F().WaitForBroadcast().has_value(), "The start goes out");

    F().ShareStatus({0x01});
    zassert_true(log.WaitForEntry()->is_active, "The unit logs");

    // Its shell stops logging: Stopped follows the active status, and it stands.
    F().ShareStatus({0x00, 0x00});
    auto status = log.WaitForEntry();
    zassert_true(status.has_value());
    zassert_equal(status->reason, LoggingStatusReason::Stopped);
    zassert_false(F().WaitForBroadcast(NO_FRAME_TIMEOUT_MS).has_value(), "A stop from the shell stands");

    // This unit's COM service restarts and forgets the network.
    zassert_true(F().harness.com->Stop());
    status = log.WaitForEntry();
    zassert_true(status.has_value());
    zassert_equal(status->reason, LoggingStatusReason::Unavailable, "Nobody is known after the restart");

    F().harness.BringOnline();
    F().harness.Settle();

    // The logging unit answers the unit's sync request with its record, still inactive.
    F().ShareStatus({0x00, 0x00});
    status = log.WaitForEntry();
    zassert_true(status.has_value());
    zassert_equal(status->reason, LoggingStatusReason::Stopped);
    zassert_true(F().WaitForBroadcast().has_value(), "so the wanted start is sent again, over the stop from the shell");

    importer.reset();
}
