#include <algorithm>
#include <utility>

#include <zephyr/logging/log.h>

#include "cdmp_status_machine.h"

LOG_MODULE_REGISTER(cdmp_status_machine, LOG_LEVEL_INF);

namespace eerie_leap::subsys::cdmp::utilities {

CdmpStatusMachine::CdmpStatusMachine(CdmpDeviceStatus initial_status) :
    status_(initial_status) {}

void CdmpStatusMachine::SetStatus(CdmpDeviceStatus new_status, bool force) {
    if(new_status == status_)
        return;

    const CdmpDeviceStatus old_status = status_;

    if(!force && !CanTransitionTo(new_status)) {
        LOG_WRN(
            "Invalid status transition from %d to %d", std::to_underlying(old_status), std::to_underlying(new_status)
        );
        return;
    }

    status_ = new_status;

    LOG_INF("Status transition: %d -> %d", std::to_underlying(old_status), std::to_underlying(new_status));
    NotifyStatusChange(old_status, new_status);
}

CdmpDeviceStatus CdmpStatusMachine::GetCurrentStatus() const {
    return status_;
}

int CdmpStatusMachine::RegisterStatusChangeHandler(StatusChangeCallback callback) {
    int max_id = 0;
    for(const auto& [id, _] : status_change_callbacks_)
        max_id = std::max(max_id, id);
    const int new_id = max_id + 1;

    status_change_callbacks_[new_id] = std::move(callback);
    return new_id;
}

void CdmpStatusMachine::UnregisterStatusChangeHandler(int callback_id) {
    status_change_callbacks_.erase(callback_id);
}

bool CdmpStatusMachine::CanTransitionTo(CdmpDeviceStatus new_status) const {
    const auto current_status = GetCurrentStatus();

    return IsValidTransition(current_status, new_status);
}

bool CdmpStatusMachine::IsOperational() const {
    const auto current_status = GetCurrentStatus();

    return current_status == CdmpDeviceStatus::ONLINE || current_status == CdmpDeviceStatus::CLAIMING;
}

void CdmpStatusMachine::NotifyStatusChange(CdmpDeviceStatus old_status, CdmpDeviceStatus new_status) {
    for(const auto& [_, callback] : status_change_callbacks_)
        callback(old_status, new_status);
}

bool CdmpStatusMachine::IsValidTransition(CdmpDeviceStatus from, CdmpDeviceStatus to) {
    switch(from) {
        case CdmpDeviceStatus::OFFLINE:
            return to == CdmpDeviceStatus::INIT || to == CdmpDeviceStatus::ERROR;

        case CdmpDeviceStatus::INIT:
            return to == CdmpDeviceStatus::CLAIMING || to == CdmpDeviceStatus::OFFLINE || to == CdmpDeviceStatus::ERROR;

        case CdmpDeviceStatus::CLAIMING:
            return to == CdmpDeviceStatus::INIT
                || to == CdmpDeviceStatus::ONLINE
                || to == CdmpDeviceStatus::VERSION_MISMATCH
                || to == CdmpDeviceStatus::ERROR;

        case CdmpDeviceStatus::ONLINE:
            return to == CdmpDeviceStatus::VERSION_MISMATCH
                || to == CdmpDeviceStatus::OFFLINE
                || to == CdmpDeviceStatus::ERROR;

        case CdmpDeviceStatus::VERSION_MISMATCH:
            return to == CdmpDeviceStatus::OFFLINE;

        case CdmpDeviceStatus::ERROR:
            return to == CdmpDeviceStatus::INIT || to == CdmpDeviceStatus::OFFLINE;

        default:
            return false;
    }
}

} // namespace eerie_leap::subsys::cdmp::utilities
