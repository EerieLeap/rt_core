#include <zephyr/logging/log.h>

#include "cdmp_canbus_service_base.h"

LOG_MODULE_REGISTER(cdmp_canbus_service, LOG_LEVEL_INF);

namespace eerie_leap::subsys::cdmp::services {

using eerie_leap::subsys::canbus::Canbus;
using eerie_leap::subsys::canbus::CanFilter;
using eerie_leap::subsys::canbus::CanFrame;

CdmpCanbusServiceBase::CdmpCanbusServiceBase(
    std::shared_ptr<CdmpCanIdManager> can_id_manager,
    std::shared_ptr<CdmpDevice> device
) :
    can_id_manager_(std::move(can_id_manager)),
    device_(std::move(device)) {

    status_handler_id_ = device_->GetStatusMachine().RegisterStatusChangeHandler(
        [this](CdmpDeviceStatus old_status, CdmpDeviceStatus new_status) {
            OnDeviceStatusChanged(old_status, new_status);
        }
    );

    if(status_handler_id_ < 0)
        throw std::runtime_error("Failed to register status change handler");
}

CdmpCanbusServiceBase::~CdmpCanbusServiceBase() {
    if(status_handler_id_ >= 0)
        device_->GetStatusMachine().UnregisterStatusChangeHandler(status_handler_id_);
}

void CdmpCanbusServiceBase::Configure(std::shared_ptr<CanbusProxy> canbus) {
    canbus_ = std::move(canbus);
}

void CdmpCanbusServiceBase::OnDeviceStatusChanged(CdmpDeviceStatus old_status, CdmpDeviceStatus new_status) {}

int CdmpCanbusServiceBase::RegisterReceiveHandler(const CanId& can_id, CanFrameHandler handler) {
    const auto for_id = [id = can_id.id, handler = std::move(handler)](const CanFrame& frame) {
        if(frame.id == id)
            handler(frame);
    };

    const CanFilter filter = can_id_manager_->GetReceiveFilter(can_id);
    int handler_id = (*canbus_)->RegisterFrameReceivedHandler(filter, for_id);

    if(handler_id == Canbus::ERR_FILTER_OVERLAP && !filter.IsExact()) {
        LOG_WRN(
            "Another CAN filter overlaps the CDMP IDs 0x%03X-0x%03X; CDMP ID 0x%03X uses its own filter.", filter.id,
            filter.id + CdmpCanIdManager::RECEIVE_BLOCK_SIZE - 1, can_id.id
        );
        handler_id = (*canbus_)->RegisterFrameReceivedHandler(CanFilter(can_id), for_id);
    }

    return handler_id;
}

} // namespace eerie_leap::subsys::cdmp::services
