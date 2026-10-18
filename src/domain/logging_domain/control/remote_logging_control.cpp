#include <exception>
#include <stdexcept>
#include <utility>
#include <variant>

#include <zephyr/logging/log.h>

#include "domain/logging_domain/event_bus/logging_event_type.h"
#include "domain/logging_domain/event_bus/logging_payload_type.h"
#include "domain/logging_domain/event_bus/logging_status_reason.h"
#include "subsys/cdmp/models/cdmp_device.h"
#include "subsys/event_bus/event_channel.h"

#include "logging_unit.h"

#include "remote_logging_control.h"

namespace eerie_leap::domain::logging_domain::control {

using eerie_leap::domain::logging_domain::event_bus::LoggingEventType;
using eerie_leap::domain::logging_domain::event_bus::LoggingPayloadType;
using eerie_leap::domain::logging_domain::event_bus::LoggingStatusReason;
using eerie_leap::subsys::cdmp::models::CdmpDeviceStatus;
using eerie_leap::subsys::event_bus::CreateScopedSubscription;

LOG_MODULE_REGISTER(remote_logging_control_logger);

namespace {

// The lock around the targets.
class ScopedSpinlock {
public:
    explicit ScopedSpinlock(k_spinlock& lock) :
        lock_(lock),
        key_(k_spin_lock(&lock)) {}
    ~ScopedSpinlock() {
        k_spin_unlock(&lock_, key_);
    }

    ScopedSpinlock(const ScopedSpinlock&) = delete;
    ScopedSpinlock& operator=(const ScopedSpinlock&) = delete;

private:
    k_spinlock& lock_;
    k_spinlock_key_t key_;
};

} // namespace

RemoteLoggingControl::RemoteLoggingControl(
    std::shared_ptr<WorkQueueThread> config_work_queue_thread,
    std::shared_ptr<ILoggingCommandSender> command_sender,
    std::shared_ptr<ILoggingRestartMonitor> restart_monitor,
    std::shared_ptr<const ICdmpNetworkInfo> network_info,
    Mode mode
) :
    config_work_queue_thread_(std::move(config_work_queue_thread)),
    command_sender_(std::move(command_sender)),
    restart_monitor_(std::move(restart_monitor)),
    network_info_(std::move(network_info)),
    mode_(mode),
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
            [](RemoteLoggingControl* control) {
                return control->Process();
            },
            this
        );
    } catch(const std::exception& e) {
        // The queue is not running.
        LOG_ERR("Failed to create the remote logging control's task: %s", e.what());
        return false;
    }

    // A peer follows no status: the writer it belongs to knows its own.
    if(mode_ == Mode::Remote) {
        // The composition root decides which bus carries the channel. Until one does, IsActive() stays
        // false and a wanted start is not sent again when a logging unit appears.
        auto& channel = LoggingEventsChannel::GetInstance();
        if(!channel.IsRegistered())
            LOG_WRN("No bus carries logging_events yet; the logging status goes unseen until one does.");

        // The importer publishes asynchronously, so this runs on the bus thread.
        try {
            status_subscription_ = CreateScopedSubscription(
                channel, LoggingEventType::StatusUpdated,
                [guard = guard_, this](const LoggingEventsChannel::EventMessage& event) {
                    guard->Dispatch([&] {
                        OnStatusUpdated(event);
                    });
                }
            );
        } catch(const std::exception& e) {
            LOG_ERR("Failed to subscribe to the logging status: %s", e.what());
        }

        if(status_subscription_ == nullptr) {
            LOG_ERR("Failed to subscribe to the logging status.");
            // Nothing was scheduled, and no request reaches a control that is not initialized.
            task_.reset();

            return false;
        }
    }

    // Last: nothing can fail after it. Setting the handler allocates, so it can throw.
    if(restart_monitor_ != nullptr) {
        try {
            restart_monitor_->SetRestartHandler([guard = guard_, this](uint8_t device_id) {
                guard->Dispatch([&] {
                    OnLoggingUnitRestarted(device_id);
                });
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
            {
                const ScopedSpinlock lock(targets_lock_);
                is_status_reassert_due_ = false;
                restarted_devices_.reset();
            }

            return false;
        }
    }

    is_initialized_ = true;

    return true;
}

void RemoteLoggingControl::RequestStart(uint32_t target_uid) {
    Submit(target_uid, Request::Start);
}

void RemoteLoggingControl::RequestStop(uint32_t target_uid) {
    Submit(target_uid, Request::Stop);
}

bool RemoteLoggingControl::HandleLoggingCommand(bool is_start) {
    // A unit without a writer starts nothing.
    if(is_start)
        return false;

    // Only starts are replaced: a stop of this unit's own that is not sent yet stays due. A stop
    // reaches this unit as a broadcast or as a unicast, and it cannot tell which, so it drops every
    // wanted start.
    bool is_dropped = false;
    {
        const ScopedSpinlock lock(targets_lock_);

        for(auto& target : targets_) {
            if(target.wanted == Request::Start) {
                target.wanted = Request::None;
                target.is_held = false;
                is_dropped = true;
            }
        }
    }

    if(is_dropped)
        LOG_INF("Another unit stopped logging; the wanted starts are dropped.");

    return true;
}

bool RemoteLoggingControl::IsActive() const {
    return is_active_;
}

// Under the lock the task takes its snapshot with, so a run that takes the due flag sees at least this
// request, and a later request sets it again.
void RemoteLoggingControl::Submit(uint32_t target_uid, Request request) {
    if(!is_initialized_) {
        LOG_ERR(
            "Logging %s requested before the remote logging control was initialized.",
            request == Request::Start ? "start" : "stop"
        );
        return;
    }

    // This unit has no writer, so it cannot be a target.
    if(target_uid != kAllUnits
        && mode_ == Mode::Remote
        && network_info_ != nullptr
        && network_info_->GetDeviceInfo().uid == target_uid) {

        LOG_WRN(
            "Logging %s for this unit (uid %u) is dropped: it has no log writer.",
            request == Request::Start ? "start" : "stop", static_cast<unsigned>(target_uid)
        );
        return;
    }

    bool is_taken = true;
    {
        const ScopedSpinlock lock(targets_lock_);

        if(target_uid == kAllUnits) {
            // The broadcast reaches every unit, so it replaces what any target wanted.
            for(auto& target : targets_)
                target = Target{};

            targets_[0].wanted = request;
            targets_[0].is_send_due = true;
        } else {
            Target* slot = nullptr;
            Target* free_slot = nullptr;
            for(size_t i = 1; i < targets_.size(); i++) {
                auto& target = targets_[i];
                if(target.uid == target_uid && target.wanted != Request::None) {
                    slot = &target;
                    break;
                }

                // Free: nothing wanted, or a stop that is already sent.
                if(free_slot == nullptr
                    && (target.wanted == Request::None || (target.wanted == Request::Stop && !target.is_send_due))) {
                    free_slot = &target;
                }
            }

            if(slot == nullptr)
                slot = free_slot;

            if(slot == nullptr) {
                is_taken = false;
            } else {
                *slot = Target{ .uid = target_uid, .wanted = request, .is_send_due = true, .is_held = false };
            }
        }
    }

    if(!is_taken) {
        LOG_ERR(
            "No room to follow logging %s for uid %u; dropped.", request == Request::Start ? "start" : "stop",
            static_cast<unsigned>(target_uid)
        );
        return;
    }

    if(task_.has_value())
        task_->Reschedule(K_NO_WAIT);
}

std::optional<uint8_t> RemoteLoggingControl::Resolve(uint32_t uid, size_t count) const {
    for(size_t i = 0; i < count; i++) {
        const auto& device = devices_[i];
        if(device.uid == uid && device.status == CdmpDeviceStatus::ONLINE && CanLog(device))
            return device.device_id;
    }

    return std::nullopt;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one pass of four ordered steps over the same run state (targets, devices, held flags); splitting it would pass that state through every part.
WorkQueueTaskResult RemoteLoggingControl::Process() {
    // What this run acts on, taken at once: a request after this schedules the next run.
    std::array<Target, K_MAX_TARGETS> targets;
    std::bitset<256> restarted;
    bool is_status_reassert_due = false;
    {
        const ScopedSpinlock lock(targets_lock_);

        targets = targets_;
        for(auto& target : targets_)
            target.is_send_due = false;

        restarted = restarted_devices_;
        restarted_devices_.reset();
        is_status_reassert_due = is_status_reassert_due_;
        is_status_reassert_due_ = false;
    }

    // The device table, once, when some UID target or restart needs it.
    size_t device_count = 0;
    if(network_info_ != nullptr) {
        bool is_needed = restarted.any();
        for(size_t i = 1; i < targets.size() && !is_needed; i++)
            is_needed = targets[i].wanted != Request::None;

        if(is_needed)
            device_count = network_info_->GetNetworkDevices(devices_);
    }

    // The units a request of their own stopped: the start for all units must not undo it. A stop slot
    // lasts until a later request takes it, which is as long as the control remembers.
    std::bitset<256> stopped_devices;
    for(size_t i = 1; i < targets.size(); i++) {
        if(targets[i].wanted != Request::Stop)
            continue;

        const auto device_id = Resolve(targets[i].uid, device_count);
        if(device_id.has_value())
            stopped_devices.set(device_id.value());
    }

    // The devices a start went to in this run, and whether the broadcast start went out: either covers
    // a re-assertion for the same unit.
    std::bitset<256> started_devices;
    bool is_broadcast_start_sent = false;
    std::array<bool, K_MAX_TARGETS> is_held{};
    for(size_t i = 0; i < targets.size(); i++)
        is_held[i] = targets[i].is_held;

    // 1. The requests. Two requests for a target before this runs send the later one; nothing wanted
    //    sends nothing: a start another unit's stop dropped before it was sent.
    for(size_t i = 0; i < targets.size(); i++) {
        const auto& target = targets[i];
        if(!target.is_send_due || target.wanted == Request::None)
            continue;

        const bool is_start = target.wanted == Request::Start;

        if(i == 0) {
            command_sender_->SendLoggingCommand(is_start, ILoggingCommandSender::kBroadcast);
            is_broadcast_start_sent = is_start;
            continue;
        }

        const auto device_id = Resolve(target.uid, device_count);
        if(device_id.has_value()) {
            command_sender_->SendLoggingCommand(is_start, device_id.value());
            is_held[i] = false;
            if(is_start)
                started_devices.set(device_id.value());
        } else if(is_start) {
            is_held[i] = true;
            LOG_INF("Logging start for uid %u is held until that unit is online.", static_cast<unsigned>(target.uid));
        } else {
            is_held[i] = false;
            LOG_WRN("Logging stop for uid %u is not sent: that unit is not online.", static_cast<unsigned>(target.uid));
        }
    }

    // 2. The status shows a logging unit again (Remote mode): one broadcast when all units want a start,
    //    else a start to each target that resolves. A unit that was stopped on its own is left out of
    //    the broadcast, so the others get a unicast each.
    if(is_status_reassert_due) {
        if(targets[0].wanted == Request::Start) {
            if(is_broadcast_start_sent) {
                // Already covered.
            } else if(stopped_devices.none()) {
                LOG_INF("A logging unit is back; sending the wanted logging start again.");
                command_sender_->SendLoggingCommand(true, ILoggingCommandSender::kBroadcast);
                is_broadcast_start_sent = true;
            } else {
                for(size_t i = 0; i < device_count; i++) {
                    const auto& device = devices_[i];
                    if(device.status != CdmpDeviceStatus::ONLINE
                        || !CanLog(device)
                        || stopped_devices.test(device.device_id)
                        || started_devices.test(device.device_id)) {
                        continue;
                    }

                    LOG_INF(
                        "A logging unit is back; sending the wanted logging start to unit %u.",
                        static_cast<unsigned>(device.device_id)
                    );
                    command_sender_->SendLoggingCommand(true, device.device_id);
                    started_devices.set(device.device_id);
                }
            }
        } else {
            for(size_t i = 1; i < targets.size(); i++) {
                if(targets[i].wanted != Request::Start)
                    continue;

                const auto device_id = Resolve(targets[i].uid, device_count);
                if(!device_id.has_value() || started_devices.test(device_id.value()))
                    continue;

                LOG_INF(
                    "A logging unit is back; sending the logging start for uid %u again.",
                    static_cast<unsigned>(targets[i].uid)
                );
                command_sender_->SendLoggingCommand(true, device_id.value());
                started_devices.set(device_id.value());
                is_held[i] = false;
            }
        }
    }

    // 3. A unit the monitor found back and idle gets one start, if it is wanted to log: by the request
    //    for all units, or by its own target (a held one is delivered here).
    if(restarted.any() && !is_broadcast_start_sent) {
        for(size_t id = 0; id < restarted.size(); id++) {
            if(!restarted.test(id) || started_devices.test(id))
                continue;

            bool is_wanted = targets[0].wanted == Request::Start && !stopped_devices.test(id);
            size_t held_index = 0;
            for(size_t i = 1; i < targets.size() && !is_wanted; i++) {
                if(targets[i].wanted != Request::Start)
                    continue;

                const auto device_id = Resolve(targets[i].uid, device_count);
                if(device_id.has_value() && device_id.value() == id) {
                    is_wanted = true;
                    held_index = i;
                }
            }

            if(!is_wanted)
                continue;

            LOG_INF("Logging unit %u is back; sending it the wanted logging start.", static_cast<unsigned>(id));
            command_sender_->SendLoggingCommand(true, static_cast<uint8_t>(id));
            started_devices.set(id);
            if(held_index != 0)
                is_held[held_index] = false;
        }
    }

    // 4. What was held or delivered, for the slots no request changed meanwhile.
    {
        const ScopedSpinlock lock(targets_lock_);

        for(size_t i = 1; i < targets.size(); i++) {
            if(targets_[i].uid == targets[i].uid
                && targets_[i].wanted == targets[i].wanted
                && targets[i].wanted != Request::None)
                targets_[i].is_held = is_held[i];
        }
    }

    return {};
}

void RemoteLoggingControl::OnStatusUpdated(const LoggingEventsChannel::EventMessage& event) {
    const auto* const active = event.payload.find(LoggingPayloadType::IsActive);
    if(active == event.payload.end() || !std::holds_alternative<bool>(active->second))
        return;

    const bool is_active = std::get<bool>(active->second);
    is_active_ = is_active;

    auto reason = LoggingStatusReason::Stopped;
    if(!is_active) {
        const auto* const reason_entry = event.payload.find(LoggingPayloadType::Reason);
        if(reason_entry != event.payload.end() && std::holds_alternative<uint32_t>(reason_entry->second))
            reason = static_cast<LoggingStatusReason>(std::get<uint32_t>(reason_entry->second));
    }

    // Exactly this transition: no logging unit was reporting, and now one does and is idle. The first
    // status has no predecessor. Active -> Stopped or a failure -> Stopped is the unit's own doing,
    // which stands, unless the unit restarted: that is the restart monitor's case, and it reports it
    // by OnLoggingUnitRestarted().
    if(!is_active && reason == LoggingStatusReason::Stopped && is_unavailable_) {
        {
            const ScopedSpinlock lock(targets_lock_);
            is_status_reassert_due_ = true;
        }
        if(task_.has_value())
            task_->Reschedule(K_NO_WAIT);
    }

    is_unavailable_ = !is_active && reason == LoggingStatusReason::Unavailable;
}

void RemoteLoggingControl::OnLoggingUnitRestarted(uint8_t device_id) {
    LOG_DBG("Logging unit %u restarted and is idle.", static_cast<unsigned>(device_id));

    // A send already due covers it, nothing wanted sends nothing, and one run sends one start to a
    // unit for any number of reports.
    {
        const ScopedSpinlock lock(targets_lock_);
        restarted_devices_.set(device_id);
    }
    if(task_.has_value())
        task_->Reschedule(K_NO_WAIT);
}

} // namespace eerie_leap::domain::logging_domain::control
