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

/** @brief One CDMP node: discovery, heartbeat and commands on a shared work queue. */
class CdmpService : public IThread, public ServiceBase<> {
public:
    /** @brief Called with the old and the new status of this unit. */
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
    /** @param uid Unique identifier of this unit, usually CdmpUid::Generate(). */
    CdmpService(CdmpDeviceType device_type, uint32_t uid);
    ~CdmpService();

    /**
     * @brief Binds the services to a bus; only while stopped.
     * @throws std::invalid_argument if @p base_can_id fails CdmpCanIdManager::IsValidBaseCanId().
     */
    void Configure(
        std::shared_ptr<CanbusProxy> canbus,
        uint32_t base_can_id = CdmpCanIdManager::DEFAULT_BASE_CAN_ID);

    // Configuration
    void SetAutoDiscovery(bool enabled);
    void SetDeviceType(CdmpDeviceType device_type);

    std::shared_ptr<CdmpDevice> GetDevice() const { return device_; }
    std::shared_ptr<CdmpCommandService> GetCommandService() const { return command_service_; }
    /** @brief The CDMP work queue, shared with the SMP-CAN transport. */
    std::shared_ptr<WorkQueueThread> GetWorkQueueThread() const { return work_queue_thread_; }

    /** @brief Other devices on the network; see CdmpNetworkService::GetNetworkDevices(). */
    size_t GetNetworkDevices(std::span<CdmpDeviceInfo> devices) const;

    /**
     * @brief Reports status changes of this unit.
     *
     * The handler runs on the thread that changes the status; read the new device ID from GetDevice().
     *
     * @return A handler ID for UnregisterStatusChangedHandler().
     */
    int RegisterStatusChangedHandler(StatusChangedHandler handler);
    void UnregisterStatusChangedHandler(int handler_id);

    // Diagnostics
    void PrintDeviceStatus() const;
};

} // namespace eerie_leap::subsys::cdmp::services
