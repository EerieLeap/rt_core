#pragma once

#include <memory>
#include <unordered_map>
#include <functional>
#include <vector>
#include <zephyr/kernel.h>

#include "subsys/canbus/canbus.h"
#include <subsys/threading/thread.h>
#include "subsys/threading/work_queue_thread.h"

#include "subsys/cdmp/utilities/cdmp_can_id_manager.h"
#include "subsys/cdmp/models/cdmp_device.h"
#include "subsys/cdmp/models/cdmp_message.h"

#include "i_cdmp_canbus_service.h"
#include "cdmp_command_service.h"
#include "cdmp_service.h"

namespace eerie_leap::subsys::cdmp::services {

using namespace eerie_leap::subsys::canbus;
using namespace eerie_leap::subsys::threading;
using namespace eerie_leap::subsys::cdmp::models;
using namespace eerie_leap::subsys::cdmp::utilities;

class CdmpService : public IThread {
private:
    std::unique_ptr<Thread> thread_;
    std::shared_ptr<WorkQueueThread> work_queue_thread_;

    std::shared_ptr<CdmpDevice> device_;
    std::shared_ptr<Canbus> canbus_;
    std::shared_ptr<CdmpCanIdManager> can_id_manager_;
    std::shared_ptr<CdmpCommandService> command_service_;

    std::vector<std::shared_ptr<ICdmpCanbusService>> canbus_services_;

    // Configuration
    atomic_t is_running_ = ATOMIC_INIT(0);
    uint32_t base_can_id_;
    bool auto_discovery_enabled_ = true;

    void ThreadEntry() override;

public:
    CdmpService(
        CdmpDeviceType device_type,
        uint32_t uid,
        uint32_t base_can_id = CdmpCanIdManager::DEFAULT_BASE_CAN_ID);
    ~CdmpService();

    // Service lifecycle
    bool Initialize();
    void Configure(std::shared_ptr<Canbus> canbus);
    void Start();
    void Reset();
    bool IsRunning() const;

    // Configuration
    void SetAutoDiscovery(bool enabled);
    void SetDeviceType(CdmpDeviceType device_type);

    std::shared_ptr<CdmpDevice> GetDevice() const { return device_; }
    std::shared_ptr<CdmpCommandService> GetCommandService() const { return command_service_; }

    // Diagnostics
    void PrintDeviceStatus() const;
};

} // namespace eerie_leap::subsys::cdmp::services
