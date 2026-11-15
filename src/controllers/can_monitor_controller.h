#pragma once

#include <memory>

#include "canbus_controller.h"
#include "domain/ble_domain/services/ble_service.h"
#include "domain/can_monitor_domain/services/can_monitor_service.h"
#include "domain/can_monitor_domain/smp/can_monitor_mgmt_group.h"

namespace eerie_leap::controllers {

using eerie_leap::domain::ble_domain::services::BleService;
using eerie_leap::domain::can_monitor_domain::services::CanMonitorService;
using eerie_leap::domain::can_monitor_domain::smp::CanMonitorMgmtGroup;

/**
 * @brief The CAN monitor on the BLE access point: sessions that stream the frames of a CAN port on the `bus`
 * characteristic, the `canmon` SMP group, and their end on a stored CAN configuration or a disconnect.
 *
 * Create it after the CAN bus controller and the BLE controller have been initialized: it takes the CAN
 * service from the one and the access point from the other.
 *
 * - Initialize() builds the CanMonitorService with the devicetree's CAN ports and a link made of the `bus`
 *   notifier, the fast-link hold and the SMP-first check of BleService; routes the `bus` completions and
 *   Ble's disconnect to it; registers it with CanbusController::RegisterDependentService(), so that a
 *   reconfiguration stops it (ending the sessions) before the buses are replaced and starts it after; and
 *   registers the group.
 * - The destructor undoes the routes and the group. The CAN bus controller keeps its reference to the
 *   service; it stays stopped there.
 */
class CanMonitorController {
private:
    CanbusController& canbus_controller_;
    BleService& ble_service_;

    std::shared_ptr<CanMonitorService> service_;
    std::unique_ptr<CanMonitorMgmtGroup> group_;
    int disconnected_handler_id_ = -1;

public:
    CanMonitorController(CanbusController& canbus_controller, BleService& ble_service);
    ~CanMonitorController();

    CanMonitorController(const CanMonitorController&) = delete;
    CanMonitorController& operator=(const CanMonitorController&) = delete;
    CanMonitorController(CanMonitorController&&) = delete;
    CanMonitorController& operator=(CanMonitorController&&) = delete;

    /** @return 0, or -1 when the service can't be had. */
    int Initialize();
    /** @return 0, or -1 when Initialize() has not succeeded. */
    int Start();

    [[nodiscard]] std::shared_ptr<CanMonitorService> GetService() const {
        return service_;
    }
};

} // namespace eerie_leap::controllers
