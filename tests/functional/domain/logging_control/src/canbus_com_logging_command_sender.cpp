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
#include "domain/logging_domain/control/canbus_com_logging_restart_monitor.h"
#include "domain/logging_domain/control/remote_logging_control.h"
#include "domain/logging_domain/event_bus/logging_network_state.h"

#include "canbus_com_loopback_harness.h"
#include "logging_control_test_support.h"
#include "logging_restart_test_support.h"

using namespace logging_control_test;
using namespace canbus_com_loopback_test;
using namespace logging_restart_test;

using eerie_leap::domain::canbus_com_domain::commands::CanbusComCommandCode;
using eerie_leap::domain::canbus_com_domain::state_bridge::NetworkStateImporter;
using eerie_leap::domain::canbus_com_domain::states::CanbusComStateType;
using eerie_leap::domain::logging_domain::control::CanbusComLoggingCommandSender;
using eerie_leap::domain::logging_domain::control::CanbusComLoggingRestartMonitor;
using eerie_leap::domain::logging_domain::control::RemoteLoggingControl;
using eerie_leap::domain::logging_domain::event_bus::LoggingNetworkState;

namespace {

constexpr uint8_t LOGGING_CODE = std::to_underlying(CanbusComCommandCode::LOGGING);
constexpr uint8_t LOGGING_STATUS_CODE = std::to_underlying(CanbusComStateType::LOGGING_STATUS);
constexpr uint8_t OTHER_ID = 3;
constexpr uint32_t OTHER_UID = 0x33333333;
constexpr uint8_t NO_SPACE = static_cast<uint8_t>(LoggingStatusReason::NoSpace);

using LoggingImporter = NetworkStateImporter<LoggingNetworkState>;

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
            LoggingEventsChannel::GetInstance(), LoggingEventType::StatusUpdated,
            [inbox = inbox_](const LoggingEventsChannel::EventMessage& event) {
                const auto active = event.payload.find(LoggingPayloadType::IsActive);
                if(active == event.payload.end() || !std::holds_alternative<bool>(active->second))
                    return;

                Entry entry{ .is_active = std::get<bool>(active->second) };
                const auto reason = event.payload.find(LoggingPayloadType::Reason);
                if(reason != event.payload.end() && std::holds_alternative<uint32_t>(reason->second))
                    entry.reason = static_cast<LoggingStatusReason>(std::get<uint32_t>(reason->second));

                {
                    ScopedMutex guard(inbox->lock);
                    inbox->entries.push_back(entry);
                }
                k_sem_give(&inbox->received);
            }
        );
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
    uint8_t next_other_version = 1;

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

    // The status record of another logging unit, device OTHER_ID, which has Join()ed.
    void ShareOtherStatus(const Bytes& data) {
        harness.SendAsPeer(STATE_CHANGE, NotificationFrame(OTHER_ID, next_other_version, LOGGING_STATUS_CODE, data));
        zassert_true(harness.WaitForAcknowledgement(OTHER_ID, next_other_version), "The unit applied the record");
        next_other_version++;
    }

    // The peer reboots: its claim again, its first heartbeat with the digest of @p records, and its next
    // record is version 1. Nothing may be sent before the restarted unit can accept it, so no LOGGING
    // broadcast comes meanwhile. See WaitForRestartTaken() for when a replay request is due.
    void RestartPeer(std::initializer_list<Record> records, bool is_sync_request_due) {
        SendRestart(harness, PEER_ID, PEER_UID, records);
        next_peer_version = 1;
        zassert_false(
            WaitForRestartTaken(harness, PEER_ID, is_sync_request_due),
            "Nothing is sent before the restarted unit can accept it"
        );
    }

    // The same for the other logging unit, device OTHER_ID, which has Join()ed.
    void RestartOther(std::initializer_list<Record> records, bool is_sync_request_due) {
        SendRestart(harness, OTHER_ID, OTHER_UID, records);
        next_other_version = 1;
        zassert_false(
            WaitForRestartTaken(harness, OTHER_ID, is_sync_request_due),
            "Nothing is sent before the restarted unit can accept it"
        );
    }

    void PeerHeartbeat(std::initializer_list<Record> records) {
        harness.SendAsPeer(HEARTBEAT, HeartbeatFrame(PEER_ID, Digest(records), LOGGER_FEATURES));
    }

    // The next LOGGING broadcast of the unit, or nothing in time.
    std::optional<RecordedFrame> WaitForBroadcast(int timeout_ms = k_wait_ms) {
        return harness.bus.Wait(COMMAND, IsLoggingBroadcastFrame, timeout_ms);
    }

    // The next LOGGING command to any unit: a restart is re-asserted to the unit that came back.
    std::optional<RecordedFrame> WaitForCommand(int timeout_ms = k_wait_ms) {
        return harness.bus.Wait(COMMAND, IsLoggingCommandFrame, timeout_ms);
    }

    // The next LOGGING command to @p device_id alone.
    std::optional<RecordedFrame> WaitForCommandTo(uint8_t device_id, int timeout_ms = k_wait_ms) {
        return harness.bus.Wait(
            COMMAND,
            [device_id](const Bytes& bytes) {
                return IsLoggingCommandTo(bytes, device_id);
            },
            timeout_ms
        );
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

    sender.SendLoggingCommand(true, kBroadcast);
    const auto start = F().WaitForBroadcast();
    zassert_true(start.has_value(), "A start must go out on the command identifier");
    zassert_equal(start->data[3], 0x01, "start");

    sender.SendLoggingCommand(false, kBroadcast);
    const auto stop = F().WaitForBroadcast();
    zassert_true(stop.has_value());
    zassert_equal(stop->data[3], 0x00, "stop");
    zassert_not_equal(stop->data[2], start->data[2], "Each command is a transaction of its own");
}

ZTEST(canbus_com_logging_command_sender, test_AResponseAfterTheSenderIsGoneIsHarmless) {
    auto sender = std::make_unique<CanbusComLoggingCommandSender>(F().harness.com);

    sender->SendLoggingCommand(true, kBroadcast);
    const auto sent = F().WaitForBroadcast();
    zassert_true(sent.has_value());

    sender.reset();

    // A late answer completes the transaction and runs the callback, which holds only the flag.
    F().harness.SendAsPeer(
        COMMAND_RESPONSE,
        CdmpCommandResponseMessage{ .source_device_id = PEER_ID,
            .command_code = LOGGING_CODE,
            .transaction_id = sent->data[2],
            .result_code = CdmpResultCode::FAILURE,
            .data = {} }
            .ToCanFrame()
    );
    k_msleep(NO_FRAME_TIMEOUT_MS);

    CanbusComLoggingCommandSender later(F().harness.com);
    later.SendLoggingCommand(false, kBroadcast);
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

namespace {

// The remote role as the controller builds it over the harness's COM service, in its order: the
// control with its restart monitor, then the status log, then the importer, so that the importer's
// first status is seen. Destroyed in the reverse order.
struct Remote {
    RemoteLoggingControl control;
    StatusLog log;
    std::unique_ptr<LoggingImporter> importer;

    Remote() :
        control(
            TestConfigQueue(),
            std::make_shared<CanbusComLoggingCommandSender>(F().harness.com),
            std::make_shared<CanbusComLoggingRestartMonitor>(F().harness.com)
        ) {

        zassert_true(control.Initialize());
        importer = std::make_unique<LoggingImporter>(F().harness.com);
    }
};

// A start on the bus, or nothing in time.
bool IsStart(const std::optional<RecordedFrame>& frame) {
    return frame.has_value() && frame->data[3] == 0x01;
}

bool IsStop(const std::optional<RecordedFrame>& frame) {
    return frame.has_value() && frame->data[3] == 0x00;
}

} // namespace

// L2's premise on the real path: the importer's Unavailable, then the Stopped a logging unit shares.
ZTEST(canbus_com_logging_command_sender, test_AStartNoUnitReceivedIsSentAgainOnceALoggingUnitReports) {
    Remote remote;

    auto status = remote.log.WaitForEntry();
    zassert_true(status.has_value(), "The importer says where it stands");
    zassert_false(status->is_active);
    zassert_equal(status->reason, LoggingStatusReason::Unavailable, "No unit shares a status");

    remote.control.RequestStart(kAllUnits);
    zassert_true(F().WaitForBroadcast().has_value(), "The start goes out");
    zassert_false(F().WaitForBroadcast(NO_FRAME_TIMEOUT_MS).has_value(), "once");

    // A logging unit appears, idle.
    F().ShareStatus({ 0x00, 0x00 });
    status = remote.log.WaitForEntry();
    zassert_true(status.has_value());
    zassert_equal(status->reason, LoggingStatusReason::Stopped, "A unit reports it is idle");
    const auto again = F().WaitForBroadcast();
    zassert_true(again.has_value(), "so the start that nobody received is sent again");
    zassert_equal(again->data[3], 0x01);

    // It logs, then stops: that is its own doing.
    F().ShareStatus({ 0x01 });
    F().ShareStatus({ 0x00, 0x00 });
    zassert_false(F().WaitForBroadcast(NO_FRAME_TIMEOUT_MS).has_value(), "and nothing is sent a third time");
}

// A logging unit that reboots within the heartbeat timeout claims its ID again with the same UID:
// CDMP reports RESTARTED, not LEFT, so its records turn stale but stay, and the status never passes
// through Unavailable. What follows depends on what the unit's first heartbeat says about its
// records, and on what its new record says:
//
// - It already holds its status record (the exporter was built before it went online), so the
//   heartbeat carries the digest of that record. The stale copy differs, nothing is withdrawn, and
//   the record that follows replaces it: the status goes from logging straight to Stopped, which the
//   status rule does not re-assert. The restart monitor does, once the copy is current.
// - It holds no record yet (digest 0), so the heartbeat withdraws the stale copy: Unavailable, then
//   Stopped when the record arrives, which the status rule re-asserts (the monitor leaves the unit
//   to it).
// - It was idle and comes back idle with the same value: no status event at all, and the copy turns
//   current by the unit's next heartbeat, which is when the monitor re-asserts.
//
// Each sends the start once.
ZTEST(canbus_com_logging_command_sender, test_AQuickRestartThatKeepsItsRecordIsReasserted) {
    Remote remote;
    zassert_true(remote.log.WaitForEntry().has_value(), "Unavailable");

    remote.control.RequestStart(kAllUnits);
    zassert_true(F().WaitForCommand().has_value(), "The start goes out");

    F().ShareStatus({ 0x01 });
    auto status = remote.log.WaitForEntry();
    zassert_true(status.has_value());
    zassert_true(status->is_active, "The unit logs");

    // The reboot; the unit's new record is version 1 again, and its heartbeat already covers it.
    F().RestartPeer({ { LOGGING_STATUS_CODE, 1, { 0x00, 0x00 } } }, true);
    F().ShareStatus({ 0x00, 0x00 });

    status = remote.log.WaitForEntry();
    zassert_true(status.has_value());
    zassert_false(status->is_active);
    zassert_equal(status->reason, LoggingStatusReason::Stopped, "Stopped follows the active status directly");

    const auto again = F().WaitForCommand();
    zassert_true(IsStart(again), "No Unavailable came between, so the restart monitor sends the start again");
    zassert_false(F().WaitForCommand(NO_FRAME_TIMEOUT_MS).has_value(), "once");

    F().PeerHeartbeat({ { LOGGING_STATUS_CODE, 1, { 0x00, 0x00 } } });
    zassert_false(
        F().WaitForCommand(NO_FRAME_TIMEOUT_MS).has_value(), "and not again at the heartbeat that confirms the copy"
    );
}

ZTEST(canbus_com_logging_command_sender, test_AQuickRestartThatAnnouncesItsRecordLaterIsReasserted) {
    Remote remote;
    zassert_true(remote.log.WaitForEntry().has_value(), "Unavailable");

    remote.control.RequestStart(kAllUnits);
    zassert_true(F().WaitForCommand().has_value(), "The start goes out");

    F().ShareStatus({ 0x01 });
    auto status = remote.log.WaitForEntry();
    zassert_true(status.has_value());
    zassert_true(status->is_active, "The unit logs");

    // The reboot, and a first heartbeat that says the unit holds no records yet.
    F().RestartPeer({}, false);
    status = remote.log.WaitForEntry();
    zassert_true(status.has_value());
    zassert_false(status->is_active);
    zassert_equal(status->reason, LoggingStatusReason::Unavailable, "The stale copy is withdrawn");

    F().ShareStatus({ 0x00, 0x00 });
    status = remote.log.WaitForEntry();
    zassert_true(status.has_value());
    zassert_equal(status->reason, LoggingStatusReason::Stopped);

    zassert_true(IsStart(F().WaitForCommand()), "Unavailable, then Stopped: the start is sent again");
    zassert_false(
        F().WaitForCommand(NO_FRAME_TIMEOUT_MS).has_value(),
        "once: the monitor left the unit to the status rule when its record returned"
    );
}

// The unit was idle before the restart, and its new record is the same value. The stale copy is
// renewed without a change, so the status does not change and nothing is published: only the
// heartbeat that confirms the copy tells that the unit is current again.
ZTEST(canbus_com_logging_command_sender, test_AQuickRestartOfAStoppedUnitIsReasserted) {
    Remote remote;
    zassert_true(remote.log.WaitForEntry().has_value(), "Unavailable");

    remote.control.RequestStart(kAllUnits);
    zassert_true(F().WaitForCommand().has_value(), "The start goes out");

    F().ShareStatus({ 0x01 });
    zassert_true(remote.log.WaitForEntry()->is_active, "The unit logs");

    // Its shell stops logging: Stopped follows the active status, and it stands.
    F().ShareStatus({ 0x00, 0x00 });
    auto status = remote.log.WaitForEntry();
    zassert_true(status.has_value());
    zassert_equal(status->reason, LoggingStatusReason::Stopped);
    zassert_false(F().WaitForCommand(NO_FRAME_TIMEOUT_MS).has_value(), "A stop from the shell stands");

    // The reboot: version 1 again, so the copy (version 2) differs from the heartbeat's digest.
    F().RestartPeer({ { LOGGING_STATUS_CODE, 1, { 0x00, 0x00 } } }, true);
    F().ShareStatus({ 0x00, 0x00 });

    zassert_false(
        remote.log.WaitForEntry(NO_FRAME_TIMEOUT_MS).has_value(), "The value did not change: no status event"
    );
    zassert_false(F().WaitForCommand(NO_FRAME_TIMEOUT_MS).has_value(), "and nothing says yet that the copy is current");

    F().PeerHeartbeat({ { LOGGING_STATUS_CODE, 1, { 0x00, 0x00 } } });
    zassert_true(
        IsStart(F().WaitForCommand()), "Its heartbeat confirms the copy: the start is sent, over the shell's stop"
    );
    zassert_false(F().WaitForCommand(NO_FRAME_TIMEOUT_MS).has_value(), "once");
}

// The restart is consumed by a first current record that is active, or a failure: the unit started by
// itself. rt_daq's writer shares Stopped when it is built, before its own start can come, so a real
// one does not take this path; the rule is for a unit whose first current record says more.
ZTEST(canbus_com_logging_command_sender, test_AUnitThatComesBackLoggingConsumesTheRestart) {
    Remote remote;
    zassert_true(remote.log.WaitForEntry().has_value(), "Unavailable");

    remote.control.RequestStart(kAllUnits);
    zassert_true(F().WaitForCommand().has_value(), "The start goes out");

    F().ShareStatus({ 0x01 });
    zassert_true(remote.log.WaitForEntry()->is_active, "The unit logs");

    // The reboot, and the unit logs again: version 1 active is the copy, so the first heartbeat
    // confirms it at once.
    F().RestartPeer({ { LOGGING_STATUS_CODE, 1, { 0x01 } } }, false);
    F().next_peer_version = 2;
    zassert_false(F().WaitForCommand(NO_FRAME_TIMEOUT_MS).has_value(), "It logs: nothing to start");

    F().ShareStatus({ 0x00, 0x00 });
    auto status = remote.log.WaitForEntry();
    zassert_true(status.has_value());
    zassert_equal(status->reason, LoggingStatusReason::Stopped);
    zassert_false(F().WaitForCommand(NO_FRAME_TIMEOUT_MS).has_value(), "and its later stop is its own doing");
}

ZTEST(canbus_com_logging_command_sender, test_AQuickRestartWithoutAWantedStartSendsNothing) {
    Remote remote;
    zassert_true(remote.log.WaitForEntry().has_value(), "Unavailable");

    F().ShareStatus({ 0x01 });
    zassert_true(remote.log.WaitForEntry()->is_active, "The unit logs");

    F().RestartPeer({ { LOGGING_STATUS_CODE, 1, { 0x00, 0x00 } } }, true);
    F().ShareStatus({ 0x00, 0x00 });
    zassert_equal(remote.log.WaitForEntry()->reason, LoggingStatusReason::Stopped);
    zassert_false(F().WaitForCommand(NO_FRAME_TIMEOUT_MS).has_value(), "Nothing was wanted");

    // A stop ends what was wanted: the next restart, confirmed at once, sends nothing either.
    remote.control.RequestStart(kAllUnits);
    zassert_true(IsStart(F().WaitForCommand()));
    remote.control.RequestStop(kAllUnits);
    zassert_true(IsStop(F().WaitForCommand()));

    F().RestartPeer({ { LOGGING_STATUS_CODE, 1, { 0x00, 0x00 } } }, false);
    F().next_peer_version = 2;
    zassert_false(F().WaitForCommand(NO_FRAME_TIMEOUT_MS).has_value(), "A stop ends what was wanted");
}

// Each restart is one re-assertion, however many claims it takes.
ZTEST(canbus_com_logging_command_sender, test_EachQuickRestartIsReassertedOnce) {
    Remote remote;
    zassert_true(remote.log.WaitForEntry().has_value(), "Unavailable");

    remote.control.RequestStart(kAllUnits);
    zassert_true(F().WaitForCommand().has_value(), "The start goes out");

    F().ShareStatus({ 0x01 });
    F().RestartPeer({ { LOGGING_STATUS_CODE, 1, { 0x00, 0x00 } } }, true);
    F().ShareStatus({ 0x00, 0x00 });
    zassert_true(IsStart(F().WaitForCommand()), "The first restart");
    zassert_false(F().WaitForCommand(NO_FRAME_TIMEOUT_MS).has_value(), "once");

    // It logs again after the start, and reboots again. The replay request is rate limited.
    F().ShareStatus({ 0x01 });
    F().RestartPeer({ { LOGGING_STATUS_CODE, 1, { 0x00, 0x00 } } }, false);
    F().ShareStatus({ 0x00, 0x00 });
    zassert_true(IsStart(F().WaitForCommand()), "The second restart");
    zassert_false(F().WaitForCommand(NO_FRAME_TIMEOUT_MS).has_value(), "once");

    // Two reboots before the unit's record is back: claim, heartbeat, claim, heartbeat.
    F().ShareStatus({ 0x01 });
    F().RestartPeer({ { LOGGING_STATUS_CODE, 1, { 0x00, 0x00 } } }, false);
    F().RestartPeer({ { LOGGING_STATUS_CODE, 1, { 0x00, 0x00 } } }, false);
    F().ShareStatus({ 0x00, 0x00 });
    zassert_true(IsStart(F().WaitForCommand()), "Two reboots in a row");
    zassert_false(F().WaitForCommand(NO_FRAME_TIMEOUT_MS).has_value(), "are one re-assertion");
}

// Another unit still logs, so the combined status does not change when the restarted unit comes back
// idle: no status event, and the monitor is the only signal. What it reports is spent, so nothing
// lingers to override a later, unrelated stop.
ZTEST(canbus_com_logging_command_sender, test_AQuickRestartWhileAnotherUnitLogsIsReasserted) {
    F().harness.Join(OTHER_ID, OTHER_UID);

    Remote remote;
    zassert_true(remote.log.WaitForEntry().has_value(), "Unavailable");

    remote.control.RequestStart(kAllUnits);
    zassert_true(F().WaitForCommand().has_value(), "The start goes out");

    F().ShareOtherStatus({ 0x01 });
    zassert_true(remote.log.WaitForEntry()->is_active, "A unit logs");
    F().ShareStatus({ 0x01 });

    F().RestartPeer({ { LOGGING_STATUS_CODE, 1, { 0x00, 0x00 } } }, true);
    F().ShareStatus({ 0x00, 0x00 });

    zassert_false(
        remote.log.WaitForEntry(NO_FRAME_TIMEOUT_MS).has_value(),
        "The other unit still logs: the status does not change"
    );
    zassert_true(IsStart(F().WaitForCommand()), "but the restarted unit lost the start: sent again");
    zassert_false(F().WaitForCommand(NO_FRAME_TIMEOUT_MS).has_value(), "once");

    // The other unit stops on its own: the status goes Stopped, and nothing is left to send.
    F().ShareOtherStatus({ 0x00, 0x00 });
    auto status = remote.log.WaitForEntry();
    zassert_true(status.has_value());
    zassert_equal(status->reason, LoggingStatusReason::Stopped);
    zassert_false(F().WaitForCommand(NO_FRAME_TIMEOUT_MS).has_value(), "nothing lingers");
}

// A logging unit that had failed comes back clean: the wanted start is sent again, which is what the
// switch asks for, and clears the failure if the unit can start.
ZTEST(canbus_com_logging_command_sender, test_AQuickRestartOfAUnitThatHadAFailureIsReasserted) {
    Remote remote;
    zassert_true(remote.log.WaitForEntry().has_value(), "Unavailable");

    remote.control.RequestStart(kAllUnits);
    zassert_true(F().WaitForCommand().has_value(), "The start goes out");

    F().ShareStatus({ 0x01 });
    zassert_true(remote.log.WaitForEntry()->is_active, "The unit logs");
    F().ShareStatus({ 0x00, NO_SPACE });
    auto status = remote.log.WaitForEntry();
    zassert_true(status.has_value());
    zassert_equal(status->reason, LoggingStatusReason::NoSpace);
    zassert_false(F().WaitForCommand(NO_FRAME_TIMEOUT_MS).has_value(), "A failure stands");

    F().RestartPeer({ { LOGGING_STATUS_CODE, 1, { 0x00, 0x00 } } }, true);
    F().ShareStatus({ 0x00, 0x00 });
    status = remote.log.WaitForEntry();
    zassert_true(status.has_value());
    zassert_equal(status->reason, LoggingStatusReason::Stopped);

    zassert_true(IsStart(F().WaitForCommand()), "The restarted unit starts clean: the start is sent again");
    zassert_false(F().WaitForCommand(NO_FRAME_TIMEOUT_MS).has_value(), "once");
}

// The logging unit's own COM service restarts (a CAN configuration applied on it): it claims its ID
// again with its UID as a reboot does, and its records survive with their versions, so its first
// heartbeat confirms the copy at once. The monitor does not tell the two apart: a reboot usually
// starts the versions at 1 again, but a status record not stamped since boot has version 1 either
// way. So a stop made on the unit's shell is overridden by the wanted start here too. Open point
// for the user: whether this transition should send.
ZTEST(canbus_com_logging_command_sender, test_ARestartOfTheLoggingUnitsOwnComServiceReassertsAStopFromTheShell) {
    Remote remote;
    zassert_true(remote.log.WaitForEntry().has_value(), "Unavailable");

    remote.control.RequestStart(kAllUnits);
    zassert_true(F().WaitForCommand().has_value(), "The start goes out");

    F().ShareStatus({ 0x01 });
    zassert_true(remote.log.WaitForEntry()->is_active, "The unit logs");
    F().ShareStatus({ 0x00, 0x00 });
    zassert_equal(remote.log.WaitForEntry()->reason, LoggingStatusReason::Stopped);
    zassert_false(F().WaitForCommand(NO_FRAME_TIMEOUT_MS).has_value(), "A stop from the shell stands");

    SendRestart(F().harness, PEER_ID, PEER_UID, { { LOGGING_STATUS_CODE, 2, { 0x00, 0x00 } } });

    zassert_true(IsStart(F().WaitForCommand()), "The first heartbeat confirms the copy: the start is sent again");
    zassert_false(F().WaitForCommand(NO_FRAME_TIMEOUT_MS).has_value(), "once");
}

// Two logging units reboot together, as on a shared supply, and announce no record yet. The status
// goes through Unavailable: the status rule sends for the first unit that is back. The other may still
// be booting and have no command handler, so the monitor sends for it when its own record is current.
ZTEST(canbus_com_logging_command_sender, test_UnitsThatRestartTogetherAreEachReassertedOnce) {
    F().harness.Join(OTHER_ID, OTHER_UID);

    Remote remote;
    zassert_true(remote.log.WaitForEntry().has_value(), "Unavailable");

    remote.control.RequestStart(kAllUnits);
    zassert_true(F().WaitForCommand().has_value(), "The start goes out");

    F().ShareOtherStatus({ 0x01 });
    zassert_true(remote.log.WaitForEntry()->is_active, "A unit logs");
    F().ShareStatus({ 0x01 });

    F().RestartPeer({}, false);
    F().RestartOther({}, false);
    auto status = remote.log.WaitForEntry();
    zassert_true(status.has_value());
    zassert_equal(status->reason, LoggingStatusReason::Unavailable, "Both copies are withdrawn");

    F().ShareStatus({ 0x00, 0x00 });
    status = remote.log.WaitForEntry();
    zassert_true(status.has_value());
    zassert_equal(status->reason, LoggingStatusReason::Stopped);
    zassert_true(IsStart(F().WaitForCommand()), "The first unit back: the status rule sends");
    zassert_false(F().WaitForCommand(NO_FRAME_TIMEOUT_MS).has_value(), "once");

    F().ShareOtherStatus({ 0x00, 0x00 });
    zassert_false(remote.log.WaitForEntry(NO_FRAME_TIMEOUT_MS).has_value(), "The status stays Stopped");
    zassert_true(IsStart(F().WaitForCommand()), "The second unit back: the monitor sends again, for it");
    zassert_false(F().WaitForCommand(NO_FRAME_TIMEOUT_MS).has_value(), "once");
}

// A logging unit joins while another one reports: the combined status does not pass through
// Unavailable, so the status rule cannot send. Which is what a unit that was away longer than the
// heartbeat timeout looks like, with another unit still there.
ZTEST(canbus_com_logging_command_sender, test_ALoggingUnitThatJoinsWhileAnotherLogsIsReasserted) {
    Remote remote;
    zassert_true(remote.log.WaitForEntry().has_value(), "Unavailable");

    remote.control.RequestStart(kAllUnits);
    zassert_true(F().WaitForCommand().has_value(), "The start goes out");

    F().ShareStatus({ 0x01 });
    zassert_true(remote.log.WaitForEntry()->is_active, "A unit logs");

    F().harness.Join(OTHER_ID, OTHER_UID);
    F().ShareOtherStatus({ 0x00, 0x00 });

    zassert_false(
        remote.log.WaitForEntry(NO_FRAME_TIMEOUT_MS).has_value(), "The unit that logs keeps the status active"
    );
    zassert_true(IsStart(F().WaitForCommand()), "but the unit that joined has not got the start: sent again");
    zassert_false(F().WaitForCommand(NO_FRAME_TIMEOUT_MS).has_value(), "once");
}

// Two logging units power up after the gauge's switch went on. The first to report ends the spell
// without any record, and the status rule sends for it. The other may still be booting, and its
// command handler is built before the record it shares, so it gets its own start when that record
// is current.
ZTEST(canbus_com_logging_command_sender, test_LoggingUnitsThatJoinTogetherAreEachReassertedOnce) {
    Remote remote;
    zassert_true(remote.log.WaitForEntry().has_value(), "Unavailable");

    remote.control.RequestStart(kAllUnits);
    zassert_true(F().WaitForCommand().has_value(), "The start goes out");

    F().harness.Join(OTHER_ID, OTHER_UID);

    F().ShareStatus({ 0x00, 0x00 });
    auto status = remote.log.WaitForEntry();
    zassert_true(status.has_value());
    zassert_equal(status->reason, LoggingStatusReason::Stopped);
    zassert_true(IsStart(F().WaitForCommand()), "The first unit to report: the status rule sends");
    zassert_false(F().WaitForCommand(NO_FRAME_TIMEOUT_MS).has_value(), "once");

    F().ShareOtherStatus({ 0x00, 0x00 });
    zassert_false(remote.log.WaitForEntry(NO_FRAME_TIMEOUT_MS).has_value(), "The status stays Stopped");
    zassert_true(IsStart(F().WaitForCommand()), "The unit that joined has not got the start: the monitor sends");
    zassert_false(F().WaitForCommand(NO_FRAME_TIMEOUT_MS).has_value(), "once");
}

// The reverse, from the plan's risks: this unit's own COM service restarts (a CAN reconfiguration
// takes CDMP out of ONLINE), which makes it forget every other unit's records. The status goes to
// Unavailable and, when the logging unit announces itself again, to Stopped, so a wanted start is
// sent again even if a stop on the logging unit's shell had ended the session.
ZTEST(canbus_com_logging_command_sender, test_ARestartOfTheUnitsOwnComServiceReassertsAStopFromTheShell) {
    Remote remote;
    zassert_true(remote.log.WaitForEntry().has_value(), "Unavailable");

    remote.control.RequestStart(kAllUnits);
    zassert_true(F().WaitForCommand().has_value(), "The start goes out");

    F().ShareStatus({ 0x01 });
    zassert_true(remote.log.WaitForEntry()->is_active, "The unit logs");

    // Its shell stops logging: Stopped follows the active status, and it stands.
    F().ShareStatus({ 0x00, 0x00 });
    auto status = remote.log.WaitForEntry();
    zassert_true(status.has_value());
    zassert_equal(status->reason, LoggingStatusReason::Stopped);
    zassert_false(F().WaitForCommand(NO_FRAME_TIMEOUT_MS).has_value(), "A stop from the shell stands");

    // This unit's COM service restarts and forgets the network.
    zassert_true(F().harness.com->Stop());
    status = remote.log.WaitForEntry();
    zassert_true(status.has_value());
    zassert_equal(status->reason, LoggingStatusReason::Unavailable, "Nobody is known after the restart");

    F().harness.BringOnline();
    F().harness.Settle();

    // The logging unit answers the unit's sync request with its record, still inactive.
    F().ShareStatus({ 0x00, 0x00 });
    status = remote.log.WaitForEntry();
    zassert_true(status.has_value());
    zassert_equal(status->reason, LoggingStatusReason::Stopped);
    zassert_true(F().WaitForCommand().has_value(), "so the wanted start is sent again, over the stop from the shell");
    zassert_false(F().WaitForCommand(NO_FRAME_TIMEOUT_MS).has_value(), "once: no monitor had a restart to report");

    // The monitor's registrations survived the restart: a quick restart of the logging unit is
    // re-asserted as before.
    F().ShareStatus({ 0x01 });
    zassert_true(remote.log.WaitForEntry()->is_active, "The unit logs");
    F().RestartPeer({ { LOGGING_STATUS_CODE, 1, { 0x00, 0x00 } } }, true);
    F().ShareStatus({ 0x00, 0x00 });
    zassert_true(IsStart(F().WaitForCommand()), "A restart after the COM restart is re-asserted");
}
