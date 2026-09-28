#pragma once

#include <memory>
#include <span>
#include <unordered_map>
#include <functional>
#include <vector>
#include <zephyr/kernel.h>

#include "subsys/canbus/canbus_proxy.hpp"
#include <subsys/threading/thread.h>
#include "subsys/threading/service_base.h"
#include "subsys/threading/work_queue_thread.h"

#include "subsys/cdmp/utilities/cdmp_can_id_manager.h"
#include "subsys/cdmp/utilities/cdmp_status_machine.h"
#include "subsys/cdmp/models/cdmp_device.h"
#include "subsys/cdmp/models/cdmp_device_info.h"
#include "subsys/cdmp/models/cdmp_message.h"

#include "i_cdmp_canbus_service.h"
#include "cdmp_command_service.h"
#include "cdmp_network_service.h"

namespace eerie_leap::subsys::cdmp::services {

using eerie_leap::subsys::canbus::CanbusProxy;
using eerie_leap::subsys::threading::IThread;
using eerie_leap::subsys::threading::Thread;
using eerie_leap::subsys::threading::ServiceBase;
using eerie_leap::subsys::threading::ServiceState;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::subsys::cdmp::utilities::CdmpCanIdManager;
using eerie_leap::subsys::cdmp::utilities::CdmpStatusMachine;
using eerie_leap::subsys::cdmp::models::CdmpDeviceType;
using eerie_leap::subsys::cdmp::models::CdmpDeviceInfo;

class CdmpService : public IThread, public ServiceBase<> {
public:
    using StatusChangedHandler = CdmpStatusMachine::StatusChangeCallback;

private:
    std::unique_ptr<Thread> thread_;
    std::shared_ptr<WorkQueueThread> work_queue_thread_;

    std::shared_ptr<CdmpDevice> device_;
    std::shared_ptr<CanbusProxy> canbus_;
    std::shared_ptr<CdmpCanIdManager> can_id_manager_;
    std::shared_ptr<CdmpNetworkService> network_service_;
    std::shared_ptr<CdmpCommandService> command_service_;

    std::vector<std::shared_ptr<ICdmpCanbusService>> canbus_services_;

    // Configuration
    bool auto_discovery_enabled_ = true;

    void ThreadEntry() override;

    bool DoInitialize() override;
    bool DoStart() override;
    bool DoStop() override;

public:
    CdmpService(CdmpDeviceType device_type, uint32_t uid);
    ~CdmpService();

    // Service lifecycle
    void Configure(
        std::shared_ptr<CanbusProxy> canbus,
        uint32_t base_can_id = CdmpCanIdManager::DEFAULT_BASE_CAN_ID);

    // Configuration
    void SetAutoDiscovery(bool enabled);
    void SetDeviceType(CdmpDeviceType device_type);

    std::shared_ptr<CdmpDevice> GetDevice() const { return device_; }
    std::shared_ptr<CdmpCommandService> GetCommandService() const { return command_service_; }

    // Other devices on the network; see CdmpNetworkService::GetNetworkDevices().
    size_t GetNetworkDevices(std::span<CdmpDeviceInfo> devices) const;

    // Runs on the thread that changes the status; read the new device ID from GetDevice().
    int RegisterStatusChangedHandler(StatusChangedHandler handler);
    void UnregisterStatusChangedHandler(int handler_id);

    // Diagnostics
    void PrintDeviceStatus() const;
};

} // namespace eerie_leap::subsys::cdmp::services
