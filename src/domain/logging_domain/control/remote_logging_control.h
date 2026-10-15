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
// It sends a wanted start once more when a logging unit appears. A start sent while no unit logs
// (the gauge's switch is turned on before the logging unit has booted, a trigger is already on at
// boot, such as a Level switch or the ignition, or the logging unit is away) is lost, and a switch
// does not change again. So it remembers that a start is wanted, from RequestStart() until
// RequestStop(), and sends it again when the status goes from inactive Unavailable to inactive
// Stopped. It never sends again on any other transition: a stop from the logging unit's shell, or a
// failure, stands until the switch moves.
//
// The importer shows that transition in these cases only:
// - a logging unit that was away longer than the heartbeat timeout (3 x 3 s);
// - a logging unit that restarts and announces its record after its first heartbeat;
// - this unit's own COM service restarting, as a CAN reconfiguration does, which drops every
//   other unit's record. The start then also overrides a stop made on the logging unit's shell
//   (test_ARestartOfTheUnitsOwnComServiceReassertsAStopFromTheShell).
// A logging unit that restarts within the heartbeat timeout with its record already in its first
// heartbeat goes from logging straight to Stopped, which is not that transition, so its start is not
// sent again (test_KnownLimitAQuickRestartThatKeepsItsRecordIsNotReasserted). Whether L2 should cover
// that restart is an open decision.
//
// Wiring, in this order: the work queue running, Initialize(), the importer, then whatever makes
// requests (the trigger set). The first status has no predecessor, so the subscription has to exist
// before the importer publishes it; a trigger set built before Initialize() has its first request
// refused, and nothing asks again for a switch that stays on.
class RemoteLoggingControl final : public ILoggingControl {
public:
    // Throws std::invalid_argument for a null argument.
    RemoteLoggingControl(
        std::shared_ptr<WorkQueueThread> config_work_queue_thread,
        std::shared_ptr<ILoggingCommandSender> command_sender);

    // Withdraws the status subscription and the task, waiting for a send in progress; sends
    // nothing. No request may come in during or after it, and it must not run inside its own task.
    // Safe after a failed Initialize().
    ~RemoteLoggingControl() override;

    // The status handler and the task refer to it.
    RemoteLoggingControl(const RemoteLoggingControl&) = delete;
    RemoteLoggingControl& operator=(const RemoteLoggingControl&) = delete;
    RemoteLoggingControl(RemoteLoggingControl&&) = delete;
    RemoteLoggingControl& operator=(RemoteLoggingControl&&) = delete;

    // Creates the task and subscribes to logging_events. Once, before any request and before the
    // importer is built, so the importer's first status is seen. False, with nothing left behind,
    // when the queue does not run or the subscription is refused or fails. Without a bus on
    // logging_events it warns; IsActive() stays false and nothing is sent again until one carries it.
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
    // The status handler's way back to this; detached first by the destructor.
    std::shared_ptr<DispatchGuard> guard_;

    std::optional<WorkQueueTask<RemoteLoggingControl>> task_;
    AnySubscription status_subscription_;

    std::atomic<bool> is_initialized_ = false;
    // The latest request, kept until the next one: a start stays wanted until a stop is requested.
    std::atomic<Request> wanted_ = Request::None;
    // Set by a request, taken by the task: send wanted_.
    std::atomic<bool> is_send_due_ = false;
    // Set by the status handler on Unavailable -> Stopped, taken by the task.
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
};

} // namespace eerie_leap::domain::logging_domain::control
