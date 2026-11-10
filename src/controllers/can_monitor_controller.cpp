#include "can_monitor_controller.h"

#include <zephyr/logging/log.h>

#include "domain/can_monitor_domain/services/can_monitor_link.h"
#include "subsys/bluetooth/ble.h"
#include "subsys/device_tree/dt_canbus.h"

namespace eerie_leap::controllers {

using eerie_leap::domain::can_monitor_domain::services::CanMonitorLink;
using eerie_leap::subsys::bluetooth::Ble;
using eerie_leap::subsys::device_tree::DtCanbus;

LOG_MODULE_REGISTER(can_monitor_controller_logger);

CanMonitorController::CanMonitorController(CanbusController& canbus_controller, BleService& ble_service) :
    canbus_controller_(canbus_controller),
    ble_service_(ble_service) {}

CanMonitorController::~CanMonitorController() {
    // The handlers refer to the service, which the CAN bus controller keeps alive: they go first.
    if(disconnected_handler_id_ >= 0)
        Ble::UnregisterDisconnectedHandler(disconnected_handler_id_);

    if(service_) {
        ble_service_.SetBusSentHandler(nullptr);
        service_->Stop();
    }

    group_.reset();
}

int CanMonitorController::Initialize() {
    if(service_)
        return 0;

    // The access point's calls are made through the service object, which outlives this controller's routes.
    BleService* ble = &ble_service_;
    CanMonitorLink link;
    link.notifier = ble->GetBusNotifier();
    link.hold_fast = [ble] {
        ble->HoldFast();
    };
    link.release_fast = [ble] {
        ble->ReleaseFast();
    };
    link.is_smp_busy = [ble] {
        return ble->IsSmpNotifying();
    };

    auto service = std::make_shared<CanMonitorService>(canbus_controller_.GetService(), DtCanbus::Get, link);
    if(!service->Initialize()) {
        LOG_ERR("Failed to initialize the CAN monitor service.");
        return -1;
    }

    CanMonitorService* raw_service = service.get();

    ble_service_.SetBusSentHandler([raw_service] {
        raw_service->OnNotificationSent();
    });

    disconnected_handler_id_ = Ble::RegisterDisconnectedHandler([raw_service](bt_conn*) {
        raw_service->OnDisconnected();
    });

    // A new CAN configuration stops the service, which ends the sessions, before the buses are replaced.
    canbus_controller_.RegisterDependentService(service);

    group_ = std::make_unique<CanMonitorMgmtGroup>(service);
    group_->Register();

    service_ = std::move(service);

    return 0;
}

int CanMonitorController::Start() {
    if(!service_)
        return -1;

    return service_->Start() ? 0 : -1;
}

} // namespace eerie_leap::controllers
