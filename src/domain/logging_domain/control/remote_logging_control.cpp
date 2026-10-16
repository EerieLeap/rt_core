#include <exception>
#include <stdexcept>
#include <utility>
#include <variant>

#include <zephyr/logging/log.h>

#include "subsys/event_bus/event_channel.h"
#include "domain/logging_domain/event_bus/logging_event_type.h"
#include "domain/logging_domain/event_bus/logging_payload_type.h"
#include "domain/logging_domain/event_bus/logging_status_reason.h"

#include "remote_logging_control.h"

namespace eerie_leap::domain::logging_domain::control {

using eerie_leap::subsys::event_bus::CreateScopedSubscription;
using eerie_leap::domain::logging_domain::event_bus::LoggingEventType;
using eerie_leap::domain::logging_domain::event_bus::LoggingPayloadType;
using eerie_leap::domain::logging_domain::event_bus::LoggingStatusReason;

LOG_MODULE_REGISTER(remote_logging_control_logger);

RemoteLoggingControl::RemoteLoggingControl(
    std::shared_ptr<WorkQueueThread> config_work_queue_thread,
    std::shared_ptr<ILoggingCommandSender> command_sender,
    std::shared_ptr<ILoggingRestartMonitor> restart_monitor)
        : config_work_queue_thread_(std::move(config_work_queue_thread)),
        command_sender_(std::move(command_sender)),
        restart_monitor_(std::move(restart_monitor)),
        guard_(std::make_shared<DispatchGuard>(this)) {

    if(config_work_queue_thread_ == nullptr || command_sender_ == nullptr)
        throw std::invalid_argument("Remote logging control needs a work queue and a command sender.");
}

RemoteLoggingControl::~RemoteLoggingControl() {
    // A status the channel already handed to a bus thread, or a restart report the monitor already
    // copied, becomes a no-op, and one in its body is waited for; neither reschedules the task
    // afterwards.
    guard_->Detach();

    // Nothing is set after a failed Initialize(), and removing a handler does not throw.
    if(restart_monitor_ != nullptr)
        restart_monitor_->SetRestartHandler(nullptr);

    status_subscription_.reset();

    // Drops a pending run and waits for one in progress, which may be inside a send. There is no
    // task when Initialize() failed or never ran.
    if(task_.has_value())
        task_->Cancel();
}

bool RemoteLoggingControl::Initialize() {
    if(is_initialized_)
        return true;

    try {
        task_ = config_work_queue_thread_->CreateTask(
            [](RemoteLoggingControl* control) { return control->Process(); }, this);
    } catch(const std::exception& e) {
        // The queue is not running.
        LOG_ERR("Failed to create the remote logging control's task: %s", e.what());
        return false;
    }

    // The composition root decides which bus carries the channel. Until one does, IsActive() stays
    // false and a wanted start is not sent again when a logging unit appears.
    auto& channel = LoggingEventsChannel::GetInstance();
    if(!channel.IsRegistered())
        LOG_WRN("No bus carries logging_events yet; the logging status goes unseen until one does.");

    // The importer publishes asynchronously, so this runs on the bus thread.
    try {
        status_subscription_ = CreateScopedSubscription(
            channel,
            LoggingEventType::StatusUpdated,
            [guard = guard_, this](const LoggingEventsChannel::EventMessage& event) {
                guard->Dispatch([&] { OnStatusUpdated(event); });
            });
    } catch(const std::exception& e) {
        LOG_ERR("Failed to subscribe to the logging status: %s", e.what());
    }

    if(status_subscription_ == nullptr) {
        LOG_ERR("Failed to subscribe to the logging status.");
        // Nothing was scheduled, and no request reaches a control that is not initialized.
        task_.reset();

        return false;
    }

    // Last: nothing can fail after it. Setting the handler allocates, so it can throw.
    if(restart_monitor_ != nullptr) {
        try {
            restart_monitor_->SetRestartHandler([guard = guard_, this](uint8_t device_id) {
                guard->Dispatch([&] { OnLoggingUnitRestarted(device_id); });
            });
        } catch(const std::exception& e) {
            LOG_ERR("Failed to listen for restarts of the logging units: %s", e.what());

            // The subscription is live, so a status may be on its way to it: Detach() waits for one
            // in its body and turns the rest into no-ops. One that went before may have scheduled the
            // task (Unavailable, then Stopped), so it is cancelled before it goes: a queued run would
            // otherwise outlive it. Nothing schedules it any more, and no request reaches a control
            // that is not initialized. The guard is replaced, so that a later Initialize() starts with
            // a live one and a clean state.
            guard_->Detach();
            status_subscription_.reset();
            task_->Cancel();
            task_.reset();
            guard_ = std::make_shared<DispatchGuard>(this);
            is_active_ = false;
            is_unavailable_ = false;
            is_reassert_due_ = false;

            return false;
        }
    }

    is_initialized_ = true;

    return true;
}

void RemoteLoggingControl::RequestStart() {
    Submit(Request::Start);
}

void RemoteLoggingControl::RequestStop() {
    Submit(Request::Stop);
}

bool RemoteLoggingControl::HandleLoggingCommand(bool is_start) {
    // A unit without a writer starts nothing.
    if(is_start)
        return false;

    // Only a start is replaced: a stop of this unit's own that is not sent yet stays due.
    auto expected = Request::Start;
    if(wanted_.compare_exchange_strong(expected, Request::None))
        LOG_INF("Another unit stopped logging; the wanted start is dropped.");

    return true;
}

bool RemoteLoggingControl::IsActive() const {
    return is_active_;
}

// The stores are ordered: the task takes the flag first and the request second, so a run that takes
// the flag sees at least this request, and a later request sets the flag again.
void RemoteLoggingControl::Submit(Request request) {
    if(!is_initialized_) {
        LOG_ERR("Logging %s requested before the remote logging control was initialized.",
            request == Request::Start ? "start" : "stop");
        return;
    }

    wanted_ = request;
    is_send_due_ = true;
    task_->Reschedule(K_NO_WAIT);
}

WorkQueueTaskResult RemoteLoggingControl::Process() {
    const bool is_send_due = is_send_due_.exchange(false);
    const bool is_reassert_due = is_reassert_due_.exchange(false);
    const Request wanted = wanted_;

    // Two requests before this runs send the later one. A re-assertion in the same run is covered by
    // that send; one after a stop finds nothing wanted; a stop requested meanwhile is sent by the run
    // it queued. Nothing wanted sends nothing: a start another unit's stop dropped before it was sent.
    if(is_send_due) {
        if(wanted != Request::None)
            command_sender_->SendLoggingCommand(wanted == Request::Start);
    } else if(is_reassert_due && wanted == Request::Start) {
        LOG_INF("A logging unit is back; sending the wanted logging start again.");
        command_sender_->SendLoggingCommand(true);
    }

    return {};
}

void RemoteLoggingControl::OnStatusUpdated(const LoggingEventsChannel::EventMessage& event) {
    const auto active = event.payload.find(LoggingPayloadType::IsActive);
    if(active == event.payload.end() || !std::holds_alternative<bool>(active->second))
        return;

    const bool is_active = std::get<bool>(active->second);
    is_active_ = is_active;

    auto reason = LoggingStatusReason::Stopped;
    if(!is_active) {
        const auto reason_entry = event.payload.find(LoggingPayloadType::Reason);
        if(reason_entry != event.payload.end() && std::holds_alternative<uint32_t>(reason_entry->second))
            reason = static_cast<LoggingStatusReason>(std::get<uint32_t>(reason_entry->second));
    }

    // Exactly this transition: no logging unit was reporting, and now one does and is idle. The first
    // status has no predecessor. Active -> Stopped or a failure -> Stopped is the unit's own doing,
    // which stands, unless the unit restarted: that is the restart monitor's case, and it reports it
    // by OnLoggingUnitRestarted().
    if(!is_active && reason == LoggingStatusReason::Stopped && is_unavailable_) {
        is_reassert_due_ = true;
        task_->Reschedule(K_NO_WAIT);
    }

    is_unavailable_ = !is_active && reason == LoggingStatusReason::Unavailable;
}

void RemoteLoggingControl::OnLoggingUnitRestarted(uint8_t device_id) {
    LOG_DBG("Logging unit %u restarted and is idle.", static_cast<unsigned>(device_id));

    // The same flag as the status rule: a send already due covers it, nothing wanted sends nothing,
    // and one run sends one start for any number of reports.
    is_reassert_due_ = true;
    task_->Reschedule(K_NO_WAIT);
}

} // namespace eerie_leap::domain::logging_domain::control
