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
    std::shared_ptr<ILoggingCommandSender> command_sender)
        : config_work_queue_thread_(std::move(config_work_queue_thread)),
        command_sender_(std::move(command_sender)),
        guard_(std::make_shared<DispatchGuard>(this)) {

    if(config_work_queue_thread_ == nullptr || command_sender_ == nullptr)
        throw std::invalid_argument("Remote logging control needs a work queue and a command sender.");
}

RemoteLoggingControl::~RemoteLoggingControl() {
    // A status the channel already handed to a bus thread becomes a no-op, and one in its body is
    // waited for; neither reschedules the task afterwards.
    guard_->Detach();

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

    is_initialized_ = true;

    return true;
}

void RemoteLoggingControl::RequestStart() {
    Submit(Request::Start);
}

void RemoteLoggingControl::RequestStop() {
    Submit(Request::Stop);
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
    const bool is_start_wanted = wanted_ == Request::Start;

    // Two requests before this runs send the later one. A re-assertion in the same run is covered by
    // that send; one after a stop finds nothing wanted; a stop requested meanwhile is sent by the run
    // it queued.
    if(is_send_due) {
        command_sender_->SendLoggingCommand(is_start_wanted);
    } else if(is_reassert_due && is_start_wanted) {
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
    // status has no predecessor, and active -> Stopped or a failure -> Stopped is the unit's own
    // doing, which stands.
    if(!is_active && reason == LoggingStatusReason::Stopped && is_unavailable_) {
        is_reassert_due_ = true;
        task_->Reschedule(K_NO_WAIT);
    }

    is_unavailable_ = !is_active && reason == LoggingStatusReason::Unavailable;
}

} // namespace eerie_leap::domain::logging_domain::control
