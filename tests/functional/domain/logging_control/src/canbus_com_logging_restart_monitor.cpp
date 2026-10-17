#include <array>
#include <deque>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include <zephyr/ztest.h>

#include "domain/canbus_com_domain/states/canbus_com_state_type.h"
#include "domain/logging_domain/control/canbus_com_logging_restart_monitor.h"
#include "domain/logging_domain/event_bus/logging_status_reason.h"

#include "canbus_com_loopback_harness.h"
#include "logging_control_test_support.h"
#include "logging_restart_test_support.h"
#include "cdmp_test_features.h"

using eerie_leap::tests::LOGGER_FEATURES;
using eerie_leap::tests::DISPLAY_FEATURES;

using namespace logging_control_test;
using namespace logging_restart_test;

using eerie_leap::domain::canbus_com_domain::states::CanbusComStateType;
using eerie_leap::domain::logging_domain::control::CanbusComLoggingRestartMonitor;

namespace {

constexpr uint8_t LOGGING_STATUS_CODE = std::to_underlying(CanbusComStateType::LOGGING_STATUS);
constexpr uint8_t OTHER_ID = 3;
constexpr uint32_t OTHER_UID = 0x33333333;
// A second ID for the peer's UID, for a unit that gives up its ID.
constexpr uint8_t SPARE_ID = 4;
// A unit that is not a logger.
constexpr uint32_t DISPLAY_UID = 0x44444444;
// A third logger.
constexpr uint8_t THIRD_ID = 5;
constexpr uint32_t THIRD_UID = 0x55555555;

constexpr uint8_t NO_SPACE = static_cast<uint8_t>(LoggingStatusReason::NoSpace);

// The record the peer's status takes: {active}, or {inactive, reason}.
const Bytes ACTIVE = {0x01};
const Bytes STOPPED = {0x00, 0x00};

// What the recording handler saw: the device IDs it was called with, in order.
struct Reports {
    k_mutex lock;
    k_sem received;
    std::deque<uint8_t> device_ids;
    // The handler throws after it recorded the call.
    bool is_throwing = false;

    Reports() {
        k_mutex_init(&lock);
        k_sem_init(&received, 0, K_SEM_MAX_LIMIT);
    }
};

// The monitor alone, with a recording handler, on the unit and a scripted peer (device 1) brought
// online. Units are scripted by device ID, with their own version counters.
struct Fixture {
    Harness harness;
    std::shared_ptr<Reports> reports = std::make_shared<Reports>();
    std::unique_ptr<CanbusComLoggingRestartMonitor> monitor;
    std::array<uint8_t, 8> next_version{};

    Fixture() {
        harness.BringOnline();
        harness.Settle();
        next_version.fill(1);

        monitor = std::make_unique<CanbusComLoggingRestartMonitor>(harness.com);
        monitor->SetRestartHandler(MakeHandler());
    }

    CanbusComLoggingRestartMonitor::RestartHandler MakeHandler() const {
        return [reports = reports](uint8_t device_id) {
            {
                ScopedMutex guard(reports->lock);
                reports->device_ids.push_back(device_id);
            }
            k_sem_give(&reports->received);

            if(reports->is_throwing)
                throw std::runtime_error("The restart handler failed.");
        };
    }

    // @p device_id's status record, as a logging unit shares it; returns once the unit applied it.
    void ShareStatus(uint8_t device_id, const Bytes& data) {
        const uint8_t version = next_version.at(device_id)++;

        harness.SendAsPeer(STATE_CHANGE, NotificationFrame(device_id, version, LOGGING_STATUS_CODE, data));
        zassert_true(harness.WaitForAcknowledgement(device_id, version), "The unit applied the record");
    }

    void ShareStatus(const Bytes& data) {
        ShareStatus(PEER_ID, data);
    }

    // The same, without waiting: for a unit whose queue is held. Returns the record's version, which
    // WaitForAcknowledgement() takes once the queue runs.
    uint8_t SendStatus(uint8_t device_id, const Bytes& data) {
        const uint8_t version = next_version.at(device_id)++;

        harness.SendAsPeer(STATE_CHANGE, NotificationFrame(device_id, version, LOGGING_STATUS_CODE, data));

        return version;
    }

    // @p device_id reboots: its claim again, its first heartbeat with the digest of @p records, and
    // its next record is version 1. Nothing is reported while it cannot accept a start.
    void Restart(
        uint8_t device_id,
        uint32_t uid,
        std::initializer_list<Record> records,
        bool is_sync_request_due,
        uint16_t features = LOGGER_FEATURES) {

        SendRestart(harness, device_id, uid, records, features);
        next_version.at(device_id) = 1;
        zassert_false(WaitForRestartTaken(harness, device_id, is_sync_request_due),
            "Nothing is sent before the restarted unit can accept it");
    }

    void Restart(std::initializer_list<Record> records, bool is_sync_request_due) {
        Restart(PEER_ID, PEER_UID, records, is_sync_request_due);
    }

    void Heartbeat(uint8_t device_id, std::initializer_list<Record> records) {
        harness.SendAsPeer(HEARTBEAT, HeartbeatFrame(device_id, Digest(records), LOGGER_FEATURES));
    }

    std::optional<uint8_t> WaitForReport(int timeout_ms = k_wait_ms) {
        if(k_sem_take(&reports->received, Ms(timeout_ms)) != 0)
            return std::nullopt;

        ScopedMutex guard(reports->lock);
        const uint8_t device_id = reports->device_ids.front();
        reports->device_ids.pop_front();

        return device_id;
    }

    // Whether no report arrives within the absence window.
    bool HasNoReport() {
        return !WaitForReport(NO_FRAME_TIMEOUT_MS).has_value();
    }
};

// Polls the unit's device table until @p device_id is in it, or is not.
bool WaitForTableEntry(Harness& harness, uint8_t device_id, bool is_present) {
    const int64_t deadline = k_uptime_get() + FRAME_TIMEOUT_MS;
    while(harness.IsInDeviceTable(device_id) != is_present) {
        if(k_uptime_get() >= deadline)
            return false;

        k_msleep(5);
    }

    return true;
}

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
    delete Current();
    Current() = nullptr;

    ResetLoopbackDevice();
}

Fixture& F() {
    return *Current();
}

} // namespace

ZTEST_SUITE(canbus_com_logging_restart_monitor, NULL, SetUp, Before, After, NULL);

// The record changed (the unit logged before the restart), so the state handler resolves it at once.
ZTEST(canbus_com_logging_restart_monitor, test_ARestartIsReportedOnceTheUnitsStatusIsCurrent) {
    F().ShareStatus(ACTIVE);
    zassert_true(F().HasNoReport(), "A status is no restart");

    F().Restart({{LOGGING_STATUS_CODE, 1, STOPPED}}, true);
    zassert_true(F().HasNoReport(), "The copy is stale until the unit says otherwise");

    F().ShareStatus(STOPPED);
    const auto report = F().WaitForReport();
    zassert_true(report.has_value(), "The unit is back and idle");
    zassert_equal(report.value(), PEER_ID);

    F().Heartbeat(PEER_ID, {{LOGGING_STATUS_CODE, 1, STOPPED}});
    zassert_true(F().HasNoReport(), "Once per restart: a later heartbeat reports nothing");
}

// The record equals the stale copy, so no state handler runs: only the heartbeat that confirms the
// copy says that the unit is current again. The copy's version differs from the restarted unit's
// (its counter started again), so the digest of its first heartbeat does not match.
ZTEST(canbus_com_logging_restart_monitor, test_AnUnchangedStatusIsConfirmedByTheHeartbeat) {
    F().ShareStatus(ACTIVE);
    F().ShareStatus(STOPPED);

    F().Restart({{LOGGING_STATUS_CODE, 1, STOPPED}}, true);

    F().ShareStatus(STOPPED);
    zassert_true(F().HasNoReport(), "The same value raises no change");

    F().Heartbeat(PEER_ID, {{LOGGING_STATUS_CODE, 1, STOPPED}});
    const auto report = F().WaitForReport();
    zassert_true(report.has_value(), "The heartbeat confirms the copy");
    zassert_equal(report.value(), PEER_ID);
    zassert_true(F().HasNoReport());
}

// A unit that restarts with the same record, version and value, is confirmed by its first heartbeat
// already: it needs no record to be current. This is also what a unit's own COM restart looks like.
ZTEST(canbus_com_logging_restart_monitor, test_AMatchingDigestConfirmsTheRestartAtOnce) {
    F().ShareStatus(STOPPED);

    SendRestart(F().harness, PEER_ID, PEER_UID, {{LOGGING_STATUS_CODE, 1, STOPPED}});

    const auto report = F().WaitForReport();
    zassert_true(report.has_value(), "The first heartbeat confirms the copy");
    zassert_equal(report.value(), PEER_ID);
    zassert_true(F().HasNoReport());
}

ZTEST(canbus_com_logging_restart_monitor, test_ARestartOfALoggerWithoutAStatusIsLeftToTheStatusRule) {
    // Restarts holding no record: with no other unit reporting, the first announcement ends a spell
    // without any copy, which the status rule covers.
    F().Restart({}, false);

    F().ShareStatus(STOPPED);
    zassert_true(F().HasNoReport(), "A logger without a copy when it restarted is the status rule's");
}

// The first heartbeat of the restarted unit says it holds no record: its copy is withdrawn, and with
// no other unit the combined status passes through Unavailable, which the status rule covers.
ZTEST(canbus_com_logging_restart_monitor, test_AStatusWithdrawnByTheFirstHeartbeatIsLeftToTheCombinedStatus) {
    F().ShareStatus(ACTIVE);

    F().Restart({}, false);

    F().ShareStatus(STOPPED);
    zassert_true(F().HasNoReport(), "The status rule re-sends, so the monitor does not");
}

// With another unit reporting, the combined status never passes through Unavailable, so the monitor
// is the only signal.
ZTEST(canbus_com_logging_restart_monitor, test_AStatusWithdrawnWhileAnotherUnitReportsIsReportedWhenBack) {
    F().harness.Join(OTHER_ID, OTHER_UID);
    F().ShareStatus(OTHER_ID, ACTIVE);
    F().ShareStatus(ACTIVE);
    zassert_true(F().HasNoReport());

    F().Restart({}, false);
    zassert_true(F().HasNoReport(), "The copy is gone, and the other unit's stays");

    F().ShareStatus(STOPPED);
    const auto report = F().WaitForReport();
    zassert_true(report.has_value(), "The restarted unit is back");
    zassert_equal(report.value(), PEER_ID, "and only it is reported");
    zassert_true(F().HasNoReport());
}

// Two units reboot together and announce no record yet, so the combined status passes through
// Unavailable. The status rule sends for the first unit to be back, which is not the monitor's to
// report; the other may still be booting and have no command handler, so it is reported when its own
// record is.
ZTEST(canbus_com_logging_restart_monitor, test_UnitsThatRestartTogetherAreReportedOneByOne) {
    F().harness.Join(OTHER_ID, OTHER_UID);
    F().ShareStatus(OTHER_ID, ACTIVE);
    F().ShareStatus(ACTIVE);

    F().Restart(PEER_ID, PEER_UID, {}, false);
    F().Restart(OTHER_ID, OTHER_UID, {}, false);
    zassert_true(F().HasNoReport(), "Neither has a record yet");

    F().ShareStatus(STOPPED);
    zassert_true(F().HasNoReport(), "The first unit back is the status rule's");

    F().ShareStatus(OTHER_ID, STOPPED);
    const auto report = F().WaitForReport();
    zassert_true(report.has_value(), "The second is reported when its record is current");
    zassert_equal(report.value(), OTHER_ID);
    zassert_true(F().HasNoReport(), "once");
}

// Both records are applied before the state handlers run, as on a busy bus, so the status rule's one
// start covers both units: the monitor reports none.
ZTEST(canbus_com_logging_restart_monitor, test_UnitsBackInOnePassAreLeftToTheStatusRule) {
    F().harness.Join(OTHER_ID, OTHER_UID);
    F().ShareStatus(OTHER_ID, ACTIVE);
    F().ShareStatus(ACTIVE);
    F().Restart(PEER_ID, PEER_UID, {}, false);
    F().Restart(OTHER_ID, OTHER_UID, {}, false);

    CdmpQueueHold hold(F().harness);
    zassert_true(hold.Close(PEER_ID), "The CDMP work queue is held");
    const uint8_t peer_version = F().SendStatus(PEER_ID, STOPPED);
    const uint8_t other_version = F().SendStatus(OTHER_ID, STOPPED);
    hold.Open();

    zassert_true(F().harness.WaitForAcknowledgement(PEER_ID, peer_version));
    zassert_true(F().harness.WaitForAcknowledgement(OTHER_ID, other_version));
    zassert_true(F().HasNoReport(), "The combined status goes from Unavailable to Stopped once, for both");
}

// The status rule does not send when the combined status is not Stopped, so the unit that is back
// and idle is the monitor's, though another unit is back too.
ZTEST(canbus_com_logging_restart_monitor, test_AUnitBackIdleWhileAnotherIsBackLoggingIsReported) {
    F().harness.Join(OTHER_ID, OTHER_UID);
    F().ShareStatus(OTHER_ID, ACTIVE);
    F().ShareStatus(ACTIVE);
    F().Restart(PEER_ID, PEER_UID, {}, false);
    F().Restart(OTHER_ID, OTHER_UID, {}, false);

    CdmpQueueHold hold(F().harness);
    zassert_true(hold.Close(PEER_ID), "The CDMP work queue is held");
    const uint8_t peer_version = F().SendStatus(PEER_ID, STOPPED);
    const uint8_t other_version = F().SendStatus(OTHER_ID, ACTIVE);
    hold.Open();

    zassert_true(F().harness.WaitForAcknowledgement(PEER_ID, peer_version));
    zassert_true(F().harness.WaitForAcknowledgement(OTHER_ID, other_version));
    const auto report = F().WaitForReport();
    zassert_true(report.has_value(), "The combined status is active: no status rule sends");
    zassert_equal(report.value(), PEER_ID, "and the idle unit is reported");
    zassert_true(F().HasNoReport(), "The unit that logs is not");
}

// The same with a failure in place of the logging unit: the status rule sends for Stopped only.
ZTEST(canbus_com_logging_restart_monitor, test_AUnitBackIdleWhileAnotherIsBackFailingIsReported) {
    F().harness.Join(OTHER_ID, OTHER_UID);
    F().ShareStatus(OTHER_ID, ACTIVE);
    F().ShareStatus(ACTIVE);
    F().Restart(PEER_ID, PEER_UID, {}, false);
    F().Restart(OTHER_ID, OTHER_UID, {}, false);

    CdmpQueueHold hold(F().harness);
    zassert_true(hold.Close(PEER_ID), "The CDMP work queue is held");
    const uint8_t peer_version = F().SendStatus(PEER_ID, STOPPED);
    const uint8_t other_version = F().SendStatus(OTHER_ID, {0x00, NO_SPACE});
    hold.Open();

    zassert_true(F().harness.WaitForAcknowledgement(PEER_ID, peer_version));
    zassert_true(F().harness.WaitForAcknowledgement(OTHER_ID, other_version));
    const auto report = F().WaitForReport();
    zassert_true(report.has_value(), "The combined status is the failure: no status rule sends");
    zassert_equal(report.value(), PEER_ID);
    zassert_true(F().HasNoReport(), "The failing unit is consumed");
}

// A record can be applied, and its unit's heartbeat handled, before the state handlers run for it. The
// status rule sends for the unit then, and the heartbeat must not report it as well.
ZTEST(canbus_com_logging_restart_monitor, test_AHeartbeatAheadOfTheStateHandlersLeavesTheUnitToTheStatusRule) {
    F().ShareStatus(ACTIVE);
    F().Restart({}, false);

    CdmpQueueHold hold(F().harness);
    zassert_true(hold.Close(PEER_ID), "The CDMP work queue is held");
    const uint8_t version = F().SendStatus(PEER_ID, STOPPED);
    F().Heartbeat(PEER_ID, {{LOGGING_STATUS_CODE, version, STOPPED}});
    hold.Open();

    zassert_true(F().harness.WaitForAcknowledgement(PEER_ID, version));
    zassert_true(F().HasNoReport(), "The status rule sends for the unit that is back");
}

// This unit's own COM service restarts and forgets the network, tracked units included, without a
// device event. The units stay armed through it, and the records that return end the spell without
// any copy: the status rule covers them. The monitor's handlers stay registered, so a unit that
// restarts afterwards is tracked again.
ZTEST(canbus_com_logging_restart_monitor, test_TheUnitsOwnComRestartLeavesTrackedUnitsToTheStatusRule) {
    F().ShareStatus(ACTIVE);
    F().Restart({{LOGGING_STATUS_CODE, 1, STOPPED}}, true);

    zassert_true(F().harness.com->Stop());
    F().harness.BringOnline();
    F().harness.Settle();

    F().next_version.fill(1);
    F().ShareStatus(STOPPED);
    zassert_true(F().HasNoReport(), "The status rule sends for the unit that returns");

    // The handlers survived: the peer restarts again and is reported once.
    F().ShareStatus(ACTIVE);
    F().Restart({{LOGGING_STATUS_CODE, 1, STOPPED}}, true);
    F().ShareStatus(STOPPED);
    const auto report = F().WaitForReport();
    zassert_true(report.has_value(), "The monitor still hears the restart after the COM restart");
    zassert_equal(report.value(), PEER_ID);
    zassert_true(F().HasNoReport(), "once");
}

ZTEST(canbus_com_logging_restart_monitor, test_AFailureAfterTheRestartIsNotReported) {
    F().ShareStatus(ACTIVE);

    F().Restart({{LOGGING_STATUS_CODE, 1, {0x00, NO_SPACE}}}, true);
    F().ShareStatus({0x00, NO_SPACE});

    zassert_true(F().HasNoReport(), "A unit that comes back failing is no unit to start");

    // The restart is consumed: the unit's later Stopped is its own doing.
    F().ShareStatus(STOPPED);
    zassert_true(F().HasNoReport());
}

// The unit started by itself and shares the same value as the stale copy. The renewal raises no state
// handler call, so it is the unit's next heartbeat that finds the copy current, active, and consumes
// the restart.
ZTEST(canbus_com_logging_restart_monitor, test_AUnitThatComesBackLoggingConsumesTheRestart) {
    F().ShareStatus(STOPPED);
    F().ShareStatus(ACTIVE);

    F().Restart({{LOGGING_STATUS_CODE, 1, ACTIVE}}, true);
    F().ShareStatus(ACTIVE);
    zassert_true(F().HasNoReport(), "It logs");

    // The heartbeat confirms the renewed copy: that is what consumes the restart.
    F().Heartbeat(PEER_ID, {{LOGGING_STATUS_CODE, 1, ACTIVE}});
    F().ShareStatus(STOPPED);
    zassert_true(F().HasNoReport(), "and its later stop is its own doing");
}

// The same renewal, then a stop before the heartbeat that would have confirmed it. The renewal was
// invisible (the value did not change), so the stop is the first change the monitor sees, and the
// unit is back and idle: it is reported, and the wanted start overrides that stop.
ZTEST(canbus_com_logging_restart_monitor, test_AStopBeforeTheHeartbeatThatConfirmsAnActiveRenewalIsReported) {
    F().ShareStatus(STOPPED);
    F().ShareStatus(ACTIVE);

    F().Restart({{LOGGING_STATUS_CODE, 1, ACTIVE}}, true);
    F().ShareStatus(ACTIVE);
    zassert_true(F().HasNoReport(), "The renewal changes nothing the monitor sees");

    F().ShareStatus(STOPPED);
    const auto report = F().WaitForReport();
    zassert_true(report.has_value(), "The stop is the first change: the unit is back and idle");
    zassert_equal(report.value(), PEER_ID);

    F().Heartbeat(PEER_ID, {{LOGGING_STATUS_CODE, 2, STOPPED}});
    zassert_true(F().HasNoReport(), "once: the heartbeat that follows reports nothing");
}

// Two restarts before the unit is current again are one report. A claim repeated without a
// heartbeat between is one restart for CDMP already, so the second restart has its heartbeat.
ZTEST(canbus_com_logging_restart_monitor, test_TwoRestartsBeforeTheRecordAreReportedOnce) {
    F().ShareStatus(ACTIVE);

    F().Restart({{LOGGING_STATUS_CODE, 1, STOPPED}}, true);
    F().Restart({{LOGGING_STATUS_CODE, 1, STOPPED}}, false);
    zassert_true(F().HasNoReport());

    F().ShareStatus(STOPPED);
    const auto report = F().WaitForReport();
    zassert_true(report.has_value());
    zassert_equal(report.value(), PEER_ID);
    zassert_true(F().HasNoReport(), "The second restart did not make a second report");
}

// Each later restart arms the unit again.
ZTEST(canbus_com_logging_restart_monitor, test_EachRestartIsReportedOnce) {
    F().ShareStatus(ACTIVE);
    F().Restart({{LOGGING_STATUS_CODE, 1, STOPPED}}, true);
    F().ShareStatus(STOPPED);
    zassert_true(F().WaitForReport().has_value(), "The first restart");

    F().ShareStatus(ACTIVE);
    // The sync request is rate limited per unit, so none is due.
    F().Restart({{LOGGING_STATUS_CODE, 1, STOPPED}}, false);
    F().ShareStatus(STOPPED);
    zassert_true(F().WaitForReport().has_value(), "The second restart");
    zassert_true(F().HasNoReport());
}

namespace {

// The same unit claims another ID, which gives up the old one: the table drops it, and its copy.
void LeaveTable(Fixture& fixture) {
    fixture.harness.SendAsPeer(MANAGEMENT, CdmpIdClaimRequestMessage{
        .claiming_device_id = SPARE_ID,
        .uid = PEER_UID,
        .protocol_version = CdmpDevice::GetCurrentProtocolVersion()}.ToCanFrame());
    zassert_true(WaitForTableEntry(fixture.harness, PEER_ID, false), "The table dropped the old ID");
}

// A unit with the given features joins: its ID claim, then its first heartbeat.
void JoinAs(Fixture& fixture, uint8_t device_id, uint32_t uid, uint16_t features) {
    fixture.harness.SendAsPeer(MANAGEMENT, CdmpIdClaimRequestMessage{
        .claiming_device_id = device_id,
        .uid = uid,
        .protocol_version = CdmpDevice::GetCurrentProtocolVersion()}.ToCanFrame());
    fixture.harness.SendAsPeer(HEARTBEAT, HeartbeatFrame(device_id, 0, features));
    zassert_true(WaitForTableEntry(fixture.harness, device_id, true), "and joins on its first heartbeat");
}

// It returns under its first ID, as a new device with the given features, on its first heartbeat.
void ReturnToTable(Fixture& fixture, uint16_t features) {
    fixture.harness.SendAsPeer(MANAGEMENT, CdmpIdClaimRequestMessage{
        .claiming_device_id = PEER_ID,
        .uid = PEER_UID,
        .protocol_version = CdmpDevice::GetCurrentProtocolVersion()}.ToCanFrame());
    fixture.harness.SendAsPeer(HEARTBEAT, HeartbeatFrame(PEER_ID, 0, features));
    zassert_true(WaitForTableEntry(fixture.harness, PEER_ID, true), "and joins on its first heartbeat");
    fixture.next_version.at(PEER_ID) = 1;
}

} // namespace

// A unit that leaves the table is forgotten. Another unit still reports, so a state handler call
// sees a copy and nothing ends a spell; only the LEFT event can clear it. It returns as a device of
// another type, which is not tracked when it joins, so only a bit left behind could report it.
ZTEST(canbus_com_logging_restart_monitor, test_AUnitThatLeavesIsForgotten) {
    F().harness.Join(OTHER_ID, OTHER_UID);
    F().ShareStatus(OTHER_ID, ACTIVE);
    F().ShareStatus(ACTIVE);

    F().Restart({{LOGGING_STATUS_CODE, 1, STOPPED}}, true);
    LeaveTable(F());
    ReturnToTable(F(), DISPLAY_FEATURES);

    F().ShareStatus(STOPPED);
    zassert_true(F().HasNoReport(), "A unit that came back after leaving is not a restart");
}

// A unit away longer than the heartbeat timeout leaves the table, and joins it again while another unit
// reports. The combined status does not pass through Unavailable, so the monitor is the only signal.
ZTEST(canbus_com_logging_restart_monitor, test_AUnitThatReturnsAfterLeavingIsReportedWhileAnotherReports) {
    F().harness.Join(OTHER_ID, OTHER_UID);
    F().ShareStatus(OTHER_ID, ACTIVE);
    F().ShareStatus(ACTIVE);

    LeaveTable(F());
    ReturnToTable(F(), LOGGER_FEATURES);
    zassert_true(F().HasNoReport(), "It has no status yet");

    F().ShareStatus(STOPPED);
    const auto report = F().WaitForReport();
    zassert_true(report.has_value(), "The unit that returned is back and idle");
    zassert_equal(report.value(), PEER_ID);
    zassert_true(F().HasNoReport(), "once");
}

// A logger that restarts without a status of its own while another unit reports: nothing passes
// through Unavailable when its status arrives, so it is armed like a logger that joins.
ZTEST(canbus_com_logging_restart_monitor, test_ARestartOfALoggerWithoutAStatusWhileAnotherReportsIsReported) {
    F().harness.Join(OTHER_ID, OTHER_UID);
    F().ShareStatus(OTHER_ID, ACTIVE);

    F().Restart({}, false);
    zassert_true(F().HasNoReport(), "It has no status yet");

    F().ShareStatus(STOPPED);
    const auto report = F().WaitForReport();
    zassert_true(report.has_value(), "The logger that restarted is idle");
    zassert_equal(report.value(), PEER_ID);
    zassert_true(F().HasNoReport(), "once");
}

// Only a logger is counted on to share a status: a unit of another type that restarts without one
// is not tracked.
ZTEST(canbus_com_logging_restart_monitor, test_ARestartOfAnotherTypeWithoutAStatusIsNotTracked) {
    F().harness.Join(OTHER_ID, OTHER_UID);
    F().ShareStatus(OTHER_ID, ACTIVE);
    JoinAs(F(), SPARE_ID, DISPLAY_UID, DISPLAY_FEATURES);

    F().Restart(SPARE_ID, DISPLAY_UID, {}, false, DISPLAY_FEATURES);
    F().ShareStatus(SPARE_ID, STOPPED);
    zassert_true(F().HasNoReport(), "A display is no logging unit");
}

// The record is what makes a unit a logging unit, whatever its type.
ZTEST(canbus_com_logging_restart_monitor, test_ARestartOfAnotherTypeThatHoldsAStatusIsTracked) {
    JoinAs(F(), SPARE_ID, DISPLAY_UID, DISPLAY_FEATURES);
    F().ShareStatus(SPARE_ID, ACTIVE);

    F().Restart(SPARE_ID, DISPLAY_UID, {{LOGGING_STATUS_CODE, 1, STOPPED}}, true, DISPLAY_FEATURES);
    F().ShareStatus(SPARE_ID, STOPPED);
    const auto report = F().WaitForReport();
    zassert_true(report.has_value(), "A unit that shared a status is a logging unit");
    zassert_equal(report.value(), SPARE_ID);
    zassert_true(F().HasNoReport(), "once");
}

// Two loggers join while no unit reports, as they do when they power up together. The status goes
// through Unavailable when the first reports, and the status rule sends for it; the other may still be
// booting and have no command handler, so the monitor reports it when its own record is current.
ZTEST(canbus_com_logging_restart_monitor, test_LoggersThatJoinTogetherAreReportedOneByOne) {
    F().harness.Join(OTHER_ID, OTHER_UID);
    F().harness.Join(THIRD_ID, THIRD_UID);
    zassert_true(F().HasNoReport(), "Neither has a status yet");

    F().ShareStatus(OTHER_ID, STOPPED);
    zassert_true(F().HasNoReport(), "The first unit to report is the status rule's");

    F().ShareStatus(THIRD_ID, STOPPED);
    const auto report = F().WaitForReport();
    zassert_true(report.has_value(), "The second is reported when its record is current");
    zassert_equal(report.value(), THIRD_ID);
    zassert_true(F().HasNoReport(), "once");
}

// A logging unit that joins while another one reports, for the first time.
ZTEST(canbus_com_logging_restart_monitor, test_AUnitThatJoinsWhileAnotherReportsIsReported) {
    F().ShareStatus(ACTIVE);

    F().harness.Join(OTHER_ID, OTHER_UID);
    zassert_true(F().HasNoReport(), "It has no status yet");

    F().ShareStatus(OTHER_ID, STOPPED);
    const auto report = F().WaitForReport();
    zassert_true(report.has_value(), "The unit that joined is idle");
    zassert_equal(report.value(), OTHER_ID);
    zassert_true(F().HasNoReport(), "once");
}

ZTEST(canbus_com_logging_restart_monitor, test_AUnitThatJoinsAndLogsConsumesTheJoin) {
    F().ShareStatus(ACTIVE);

    F().harness.Join(OTHER_ID, OTHER_UID);
    F().ShareStatus(OTHER_ID, ACTIVE);
    // The state handlers have run for the first record before the second comes; two records in one
    // pass would show the monitor the second only.
    zassert_true(F().HasNoReport(), "It logs");

    F().ShareStatus(OTHER_ID, STOPPED);
    zassert_true(F().HasNoReport(), "and its later stop is its own doing");
}

// With no unit reporting, the status goes through Unavailable when a unit that joined reports, and the
// status rule sends: the unit is armed, and its record, which ends that spell, clears it.
ZTEST(canbus_com_logging_restart_monitor, test_AUnitThatJoinsWhileNoneReportsIsLeftToTheStatusRule) {
    F().harness.Join(OTHER_ID, OTHER_UID);

    F().ShareStatus(OTHER_ID, STOPPED);
    zassert_true(F().HasNoReport());

    F().ShareStatus(ACTIVE);
    F().ShareStatus(STOPPED);
    zassert_true(F().HasNoReport(), "Neither was armed");
}

// A control without a status rule (the network half of a writer) has nothing to leave the unit to, so
// the monitor reports the unit that ends the spell without any copy.
ZTEST(canbus_com_logging_restart_monitor, test_WithoutAStatusRuleTheMonitorReportsAReturningUnit) {
    F().monitor.reset();
    F().monitor = std::make_unique<CanbusComLoggingRestartMonitor>(F().harness.com, false);
    F().monitor->SetRestartHandler(F().MakeHandler());

    F().harness.Join(OTHER_ID, OTHER_UID);

    F().ShareStatus(OTHER_ID, STOPPED);
    const auto report = F().WaitForReport();
    zassert_true(report.has_value(), "No rule sends for it, so the monitor does");
    zassert_equal(report.value(), OTHER_ID);
}

ZTEST(canbus_com_logging_restart_monitor, test_NoReportAfterTheHandlerIsRemovedOrTheMonitorIsGone) {
    F().ShareStatus(ACTIVE);

    F().monitor->SetRestartHandler(nullptr);
    F().Restart({{LOGGING_STATUS_CODE, 1, STOPPED}}, true);
    F().ShareStatus(STOPPED);
    zassert_true(F().HasNoReport(), "Reports without a handler are dropped");

    // A new handler hears the next restart.
    F().monitor->SetRestartHandler(F().MakeHandler());
    F().ShareStatus(ACTIVE);
    F().Restart({{LOGGING_STATUS_CODE, 1, STOPPED}}, false);
    F().ShareStatus(STOPPED);
    zassert_true(F().WaitForReport().has_value(), "A handler set later is called");

    // A monitor that is gone unregistered everything.
    F().ShareStatus(ACTIVE);
    F().monitor.reset();
    F().Restart({{LOGGING_STATUS_CODE, 1, STOPPED}}, false);
    F().ShareStatus(STOPPED);
    zassert_true(F().HasNoReport(), "and calls nothing");
}

// A handler that throws would skip the heartbeat handlers after the monitor's, so a second one,
// registered after it, still has to see the heartbeat.
ZTEST(canbus_com_logging_restart_monitor, test_AHandlerThatThrowsDoesNotSkipTheHandlersAfterIt) {
    // Unwinding the throw on the CDMP work queue needs more stack than its default, which overflowed
    // on the 64 bit QEMU targets and on MIPS. native_sim does not enforce thread stacks, so elsewhere
    // the case runs only with a raised stack (functional.logging_control.raised_cdmp_stack).
    if(!IS_ENABLED(CONFIG_ARCH_POSIX) && CONFIG_EERIE_LEAP_CDMP_WORK_QUEUE_STACK_SIZE < 8192)
        ztest_test_skip();

    struct Heartbeats {
        k_sem received;
        Heartbeats() { k_sem_init(&received, 0, K_SEM_MAX_LIMIT); }
    };
    auto heartbeats = std::make_shared<Heartbeats>();
    const int handler_id = F().harness.com->RegisterHeartbeatHandler(
        [heartbeats](const CdmpHeartbeatMessage&) { k_sem_give(&heartbeats->received); });
    zassert_true(handler_id >= 0);

    F().ShareStatus(STOPPED);
    F().reports->is_throwing = true;

    // The first heartbeat confirms the copy, and the monitor reports from inside the heartbeat round.
    SendRestart(F().harness, PEER_ID, PEER_UID, {{LOGGING_STATUS_CODE, 1, STOPPED}});
    zassert_true(F().WaitForReport().has_value(), "The handler was called, and threw");
    zassert_equal(k_sem_take(&heartbeats->received, Ms(k_wait_ms)), 0,
        "The handler registered after the monitor's still saw the heartbeat");

    // The monitor goes on: another restart is reported. The first restart's digest matched, so this is
    // the first that asks for a replay.
    F().ShareStatus(ACTIVE);
    F().Restart({{LOGGING_STATUS_CODE, 1, STOPPED}}, true);
    F().ShareStatus(STOPPED);
    zassert_true(F().WaitForReport().has_value(), "The monitor survived the exception");

    F().harness.com->UnregisterHeartbeatHandler(handler_id);
}

ZTEST(canbus_com_logging_restart_monitor, test_ANullServiceIsRefused) {
    bool is_refused = false;
    try {
        CanbusComLoggingRestartMonitor monitor(nullptr);
    } catch(const std::invalid_argument&) {
        is_refused = true;
    }

    zassert_true(is_refused, "A monitor without the COM service could hear nothing");
}

// A DAQ joins with LOGGING clear (its writer is built after its first heartbeat) and shares its record
// first, while another unit reports: nothing arms it until a heartbeat carries the bit.
ZTEST(canbus_com_logging_restart_monitor, test_AUnitThatGainsLoggingAfterItsRecordIsReported) {
    F().ShareStatus(ACTIVE);

    JoinAs(F(), OTHER_ID, OTHER_UID, DISPLAY_FEATURES);
    F().ShareStatus(OTHER_ID, STOPPED);
    zassert_true(F().HasNoReport(), "It cannot log as far as anyone knows");

    F().Heartbeat(OTHER_ID, {{LOGGING_STATUS_CODE, 1, STOPPED}});
    const auto report = F().WaitForReport();
    zassert_true(report.has_value(), "The heartbeat with LOGGING arms it, and its record is current");
    zassert_equal(report.value(), OTHER_ID);

    F().Heartbeat(OTHER_ID, {{LOGGING_STATUS_CODE, 1, STOPPED}});
    zassert_true(F().HasNoReport(), "once");
}

// The heartbeat with LOGGING comes first, the record after it.
ZTEST(canbus_com_logging_restart_monitor, test_AUnitThatGainsLoggingBeforeItsRecordIsReported) {
    F().ShareStatus(ACTIVE);

    JoinAs(F(), OTHER_ID, OTHER_UID, DISPLAY_FEATURES);
    F().Heartbeat(OTHER_ID, {});
    zassert_true(F().HasNoReport(), "It has no record yet");

    F().ShareStatus(OTHER_ID, STOPPED);
    const auto report = F().WaitForReport();
    zassert_true(report.has_value(), "Its record ends the wait");
    zassert_equal(report.value(), OTHER_ID);
    zassert_true(F().HasNoReport(), "once");
}
