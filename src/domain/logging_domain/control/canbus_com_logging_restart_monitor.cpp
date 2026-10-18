#include <exception>
#include <stdexcept>
#include <utility>

#include <zephyr/logging/log.h>

#include "subsys/threading/scoped_mutex.h"

#include "domain/canbus_com_domain/states/canbus_com_logging_state.h"
#include "domain/canbus_com_domain/states/canbus_com_state_type.h"
#include "domain/logging_domain/event_bus/logging_network_state.h"
#include "domain/logging_domain/event_bus/logging_status_reason.h"
#include "domain/system_domain/models/product_feature.h"

#include "canbus_com_logging_restart_monitor.h"

namespace eerie_leap::domain::logging_domain::control {

using eerie_leap::domain::canbus_com_domain::states::CanbusComLoggingState;
using eerie_leap::domain::canbus_com_domain::states::CanbusComStateType;
using eerie_leap::domain::logging_domain::event_bus::LoggingNetworkState;
using eerie_leap::domain::logging_domain::event_bus::LoggingStatusReason;
using eerie_leap::domain::system_domain::models::HasFeature;
using eerie_leap::domain::system_domain::models::ProductFeature;
using eerie_leap::subsys::threading::ScopedMutex;

LOG_MODULE_REGISTER(canbus_com_logging_restart_monitor_logger);

CanbusComLoggingRestartMonitor::CanbusComLoggingRestartMonitor(
    std::shared_ptr<CanbusComService> canbus_com_service,
    bool has_status_rule
) :
    canbus_com_service_(std::move(canbus_com_service)),
    has_status_rule_(has_status_rule) {

    if(canbus_com_service_ == nullptr)
        throw std::invalid_argument("The logging restart monitor needs the COM service.");

    // First: a handler can run on the CDMP work queue as soon as it is registered, and the state
    // handler's first call is scheduled at once.
    k_mutex_init(&handler_lock_);

    // The units already online are the baseline of the LOGGING bit: only a unit that gains it later is
    // armed by a heartbeat. Before the handlers, which use the bitset on the CDMP work queue.
    if(const auto network_info = canbus_com_service_->GetNetworkInfo(); network_info != nullptr) {
        std::array<CdmpDeviceInfo, 64> devices{};
        const size_t count = network_info->GetNetworkDevices(devices);
        for(size_t i = 0; i < count; i++)
            has_logging_.set(devices[i].device_id, HasFeature(devices[i].features, ProductFeature::LOGGING));
    }

    // A constructor that throws runs no destructor, so it withdraws what it registered itself.
    try {
        device_event_handler_id_ = canbus_com_service_->RegisterDeviceEventHandler(
            [this](CdmpDeviceEvent event, const CdmpDeviceInfo& device) {
                OnDeviceEvent(event, device);
            }
        );
        if(device_event_handler_id_ < 0)
            throw std::runtime_error("The logging restart monitor could not register its device event handler.");

        heartbeat_handler_id_ = canbus_com_service_->RegisterHeartbeatHandler(
            [this](const CdmpHeartbeatMessage& heartbeat) {
                OnHeartbeat(heartbeat);
            }
        );
        if(heartbeat_handler_id_ < 0)
            throw std::runtime_error("The logging restart monitor could not register its heartbeat handler.");

        state_handler_id_ = canbus_com_service_->RegisterStateHandler(
            CanbusComStateType::LOGGING_STATUS, [this](CanbusComStateType /*state_type*/) {
                OnStatesChanged();
            }
        );
        if(state_handler_id_ < 0)
            throw std::runtime_error("The logging restart monitor could not register its state handler.");
    } catch(...) {
        Unregister();
        throw;
    }
}

CanbusComLoggingRestartMonitor::~CanbusComLoggingRestartMonitor() {
    Unregister();
}

void CanbusComLoggingRestartMonitor::Unregister() {
    // Each waits for a handler of its kind in progress, so none is running once all returned.
    if(state_handler_id_ >= 0)
        canbus_com_service_->UnregisterStateHandler(state_handler_id_);
    if(heartbeat_handler_id_ >= 0)
        canbus_com_service_->UnregisterHeartbeatHandler(heartbeat_handler_id_);
    if(device_event_handler_id_ >= 0)
        canbus_com_service_->UnregisterDeviceEventHandler(device_event_handler_id_);

    state_handler_id_ = -1;
    heartbeat_handler_id_ = -1;
    device_event_handler_id_ = -1;
}

void CanbusComLoggingRestartMonitor::SetRestartHandler(RestartHandler handler) {
    // Allocated before the lock; the old handler is released after it, as it is declared first.
    std::shared_ptr<const RestartHandler> slot;
    if(handler)
        slot = std::make_shared<const RestartHandler>(std::move(handler));

    ScopedMutex guard(handler_lock_);

    handler_.swap(slot);
}

void CanbusComLoggingRestartMonitor::OnDeviceEvent(CdmpDeviceEvent event, const CdmpDeviceInfo& device) {
    switch(event) {
        case CdmpDeviceEvent::RESTARTED:
        case CdmpDeviceEvent::JOINED:
            has_logging_.set(device.device_id, HasFeature(device.features, ProductFeature::LOGGING));
            if(IsLoggingUnit(device)) {
                restarted_.set(device.device_id);
                LOG_INF(
                    "Logging unit %u %s.", static_cast<unsigned>(device.device_id),
                    event == CdmpDeviceEvent::RESTARTED ? "restarted" : "joined"
                );
            }
            break;

        case CdmpDeviceEvent::LEFT:
            restarted_.reset(device.device_id);
            has_logging_.reset(device.device_id);
            break;
    }
}

bool CanbusComLoggingRestartMonitor::IsLoggingUnit(const CdmpDeviceInfo& device) {
    // A logger may share a status, and so may whatever shared one before it restarted; its copy is
    // still there, stale (the state service ran first). A unit that joined has no copy yet, so its
    // features are all there is to go by (a unit whose features carry LOGGING has a log writer). It
    // is armed whether or not another unit reports: where none does, its status ends a spell without any copy, which the status rule covers, and where one
    // does, the monitor is the only signal. A logger that never shares a status stays armed until it
    // leaves the table, which costs a bit.
    if(HasFeature(device.features, ProductFeature::LOGGING))
        return true;

    const size_t count = canbus_com_service_->GetRemoteStates(CanbusComStateType::LOGGING_STATUS, records_);
    for(size_t i = 0; i < count; i++) {
        if(records_[i].device_id == device.device_id)
            return true;
    }

    return false;
}

void CanbusComLoggingRestartMonitor::OnHeartbeat(const CdmpHeartbeatMessage& heartbeat) {
    // A unit that gains LOGGING (a DAQ whose writer is built after its first heartbeat, or that has a
    // writer again) is armed, as one that joined with it: its record may be current already, or may
    // follow, and it resolves like the others. The features at the join or restart are the baseline.
    const bool has_logging = HasFeature(heartbeat.features, ProductFeature::LOGGING);
    if(has_logging && !has_logging_.test(heartbeat.device_id)) {
        restarted_.set(heartbeat.device_id);
        LOG_INF("Unit %u gained the logging feature.", static_cast<unsigned>(heartbeat.device_id));
    }
    has_logging_.set(heartbeat.device_id, has_logging);

    // While no unit has a copy, a record can be applied, and its unit's heartbeat handled, before the
    // state handler call that ends the spell and leaves the unit to the status rule: a report here
    // would be a second start.
    if(!restarted_.test(heartbeat.device_id) || (has_status_rule_ && is_status_empty_))
        return;

    // The state service's heartbeat handler ran first (it registered first), so a digest that
    // matches has cleared the copy's stale mark by now. Were it not so, the unit's next heartbeat
    // resolves it: latency only.
    const size_t count = canbus_com_service_->GetRemoteStates(CanbusComStateType::LOGGING_STATUS, records_);
    Resolve(count, heartbeat.device_id, false);
}

void CanbusComLoggingRestartMonitor::OnStatesChanged() {
    // Read in the same notification run as the importer reads them, so the two agree: no copy at all
    // is Unavailable there. Kept current whether or not a unit is armed.
    const size_t count = canbus_com_service_->GetRemoteStates(CanbusComStateType::LOGGING_STATUS, records_);
    if(count == 0) {
        is_status_empty_ = true;
        return;
    }

    const bool is_status_returning = is_status_empty_;
    is_status_empty_ = false;

    if(restarted_.any())
        Resolve(count, std::nullopt, is_status_returning);
}

bool CanbusComLoggingRestartMonitor::IsCombinedStatusStopped(size_t count) const {
    for(size_t i = 0; i < count; i++) {
        const CanbusComLoggingState state(records_[i].data.GetData());
        if(state.IsActive() || LoggingNetworkState::IsFailure(state.GetReason()))
            return false;
    }

    return count > 0;
}

void CanbusComLoggingRestartMonitor::Resolve(size_t count, std::optional<uint8_t> device_id, bool is_status_returning) {
    // The status passes from Unavailable to Stopped in this call, and RemoteLoggingControl sends for
    // it, to every unit that can take the command: those with a copy. Reporting them too would send
    // twice. Anything else the status shows, active or a failure, sends nothing.
    const bool is_left_to_status_rule = has_status_rule_ && is_status_returning && IsCombinedStatusStopped(count);

    for(size_t i = 0; i < count; i++) {
        const RemoteStateRecord& record = records_[i];
        if(!restarted_.test(record.device_id))
            continue;
        if(device_id.has_value() && record.device_id != device_id.value())
            continue;

        if(is_left_to_status_rule) {
            restarted_.reset(record.device_id);
            LOG_DBG(
                "Logging unit %u is back with the status: the status rule sends.",
                static_cast<unsigned>(record.device_id)
            );
            continue;
        }

        if(record.is_stale)
            continue;

        restarted_.reset(record.device_id);

        const CanbusComLoggingState state(record.data.GetData());
        if(!state.IsActive() && state.GetReason() == LoggingStatusReason::Stopped)
            Report(record.device_id);
        else
            LOG_DBG(
                "Logging unit %u is back and %s.", static_cast<unsigned>(record.device_id),
                state.IsActive() ? "logs" : "does not report Stopped"
            );
    }
}

void CanbusComLoggingRestartMonitor::Report(uint8_t device_id) {
    LOG_INF("Logging unit %u is back after its restart.", static_cast<unsigned>(device_id));

    std::shared_ptr<const RestartHandler> handler;
    {
        ScopedMutex guard(handler_lock_);

        handler = handler_;
    }

    if(handler == nullptr)
        return;

    // The caller is a notification loop of the network service, which would skip the handlers after
    // this one, or of the state service.
    try {
        (*handler)(device_id);
    } catch(const std::exception& e) {
        LOG_ERR("The restart handler for logging unit %u threw: %s", static_cast<unsigned>(device_id), e.what());
    } catch(...) {
        LOG_ERR("The restart handler for logging unit %u threw.", static_cast<unsigned>(device_id));
    }
}

} // namespace eerie_leap::domain::logging_domain::control
