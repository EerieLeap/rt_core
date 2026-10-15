#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

#include "subsys/event_bus/dispatch_guard.h"
#include "subsys/event_bus/i_scoped_subscription.h"
#include "subsys/threading/work_queue_task_result.h"
#include "subsys/threading/work_queue_thread.h"
#include "domain/logging_domain/event_bus/logging_events_channel.h"

#include "i_logging_command_sender.h"
#include "i_logging_control.h"
#include "i_logging_restart_monitor.h"

namespace eerie_leap::domain::logging_domain::control {

using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::event_bus::DispatchGuard;
using eerie_leap::subsys::threading::WorkQueueTask;
using eerie_leap::subsys::threading::WorkQueueTaskResult;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::domain::logging_domain::event_bus::LoggingEventsChannel;

// Logging control of a unit without a log writer: a request becomes the CDMP LOGGING broadcast, sent
// from the configuration work queue, and IsActive() follows the logging status the units share
// (StatusUpdated on logging_events, published by the NetworkStateImporter).
//
// It sends a wanted start once more when a logging unit appears or comes back from a restart. A
// start sent while no unit logs (the gauge's switch is turned on before the logging unit has booted,
// a trigger is already on at boot, such as a Level switch or the ignition, or the logging unit is
// away or rebooting) is lost, and a switch does not change again. So it remembers that a start is
// wanted, from RequestStart() until RequestStop(), and sends it again on two signals and on no other:
// - the status goes from inactive Unavailable to inactive Stopped, which is a logging unit
//   appearing;
// - the restart monitor reports a logging unit that restarted or joined and shares an inactive
//   Stopped status again.
// A stop from the logging unit's shell, or a failure, stands until the switch moves, except where
// one of these follows it.
//
// What sends it again:
// - a logging unit that was away longer than the heartbeat timeout (3 x 3 s) and returns, or one
//   that joins: the status, or the monitor when another unit reports, so the status does not pass
//   through Unavailable;
// - a logging unit that restarts and announces its record after its first heartbeat: the status,
//   or the monitor when another unit still reports;
// - a logging unit that restarts within the heartbeat timeout and holds its record in its first
//   heartbeat: the monitor, once its record is current and Stopped. That is at once when the unit
//   logged before, and at its next heartbeat (up to 3 s) when it was idle, since its record is
//   then unchanged. It also overrides a stop made on its shell before the restart;
// - several logging units that restart or join together: the status for the first to be back, and
//   the monitor for each of the others when its record is, one start each;
// - a logging unit whose own COM service restarts (a CAN configuration applied on it), which the
//   monitor cannot tell from a reboot: as above;
// - this unit's own COM service restarting, which drops every other unit's record: the status,
//   and the start also overrides a stop made on the logging unit's shell.
//
// A restart is consumed without a start when the monitor first finds the unit's record current and
// it is active (the unit started by itself) or a failure. A record equal to the stale copy is
// found at the unit's next heartbeat, up to 3 s later: a stop in between is reported and the wanted
// start overrides it. A log writer shares Stopped when it is built, before its own triggers,
// autostart or a failed start can say more, so a restarted writer gets at most one start, and
// normally one. That is redundant when it starts by itself, and fails again when what failed before
// fails still.
//
// The tests that pin each case are in canbus_com_logging_command_sender.cpp and
// canbus_com_logging_restart_monitor.cpp.
//
// Wiring, in this order: the work queue running, the monitor, Initialize(), the importer, then
// whatever makes requests (the trigger set). The first status has no predecessor, so the
// subscription has to exist before the importer publishes it; a trigger set built before
// Initialize() has its first request refused, and nothing asks again for a switch that stays on. The
// monitor reports nothing to the control before Initialize() sets its handler.
class RemoteLoggingControl final : public ILoggingControl {
public:
    // Throws std::invalid_argument for a null queue or sender. The restart monitor may be null: the
    // control then re-sends on the status alone. The control owns it, and the controller always
    // passes one.
    RemoteLoggingControl(
        std::shared_ptr<WorkQueueThread> config_work_queue_thread,
        std::shared_ptr<ILoggingCommandSender> command_sender,
        std::shared_ptr<ILoggingRestartMonitor> restart_monitor = nullptr);

    // Withdraws the restart handler, the status subscription and the task, waiting for a send in
    // progress; sends nothing. No request may come in during or after it, and it must not run inside
    // its own task. Safe after a failed Initialize().
    ~RemoteLoggingControl() override;

    // The status handler and the task refer to it.
    RemoteLoggingControl(const RemoteLoggingControl&) = delete;
    RemoteLoggingControl& operator=(const RemoteLoggingControl&) = delete;
    RemoteLoggingControl(RemoteLoggingControl&&) = delete;
    RemoteLoggingControl& operator=(RemoteLoggingControl&&) = delete;

    // Creates the task, subscribes to logging_events and sets the restart handler. Once, before any
    // request and before the importer is built, so the importer's first status is seen. False, with
    // nothing left behind, when the queue does not run or the subscription is refused or fails, or
    // the monitor cannot take its handler. Without a bus on logging_events it warns; IsActive() stays
    // false and nothing is sent again on the status until one carries it.
    bool Initialize();

    void RequestStart() override;
    void RequestStop() override;
    [[nodiscard]] bool IsActive() const override;

private:
    enum class Request : uint8_t {
        None,
        Start,
        Stop
    };

    // Declared before task_, so the queue outlives the task.
    std::shared_ptr<WorkQueueThread> config_work_queue_thread_;
    std::shared_ptr<ILoggingCommandSender> command_sender_;
    std::shared_ptr<ILoggingRestartMonitor> restart_monitor_;
    // The status handler's and the restart handler's way back to this; detached first by the
    // destructor. The handlers run on the bus thread and on the CDMP work queue.
    std::shared_ptr<DispatchGuard> guard_;

    std::optional<WorkQueueTask<RemoteLoggingControl>> task_;
    AnySubscription status_subscription_;

    std::atomic<bool> is_initialized_ = false;
    // The latest request, kept until the next one: a start stays wanted until a stop is requested.
    std::atomic<Request> wanted_ = Request::None;
    // Set by a request, taken by the task: send wanted_.
    std::atomic<bool> is_send_due_ = false;
    // Set by the status handler on Unavailable -> Stopped and by the restart handler, taken by the
    // task.
    std::atomic<bool> is_reassert_due_ = false;
    // From the last StatusUpdated.
    std::atomic<bool> is_active_ = false;
    // Whether the last StatusUpdated was inactive Unavailable. The status handler only, inside guard_.
    bool is_unavailable_ = false;

    void Submit(Request request);

    // The task; configuration work queue only.
    WorkQueueTaskResult Process();

    // The bus thread that carries logging_events, inside guard_.
    void OnStatusUpdated(const LoggingEventsChannel::EventMessage& event);

    // The monitor's thread, the CDMP work queue, inside guard_.
    void OnLoggingUnitRestarted(uint8_t device_id);
};

} // namespace eerie_leap::domain::logging_domain::control
