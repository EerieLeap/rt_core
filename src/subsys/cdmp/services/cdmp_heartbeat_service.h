#pragma once

#include <span>
#include <optional>

#include <zephyr/kernel.h>

#include "subsys/threading/work_queue_thread.h"

#include "cdmp_canbus_service_base.h"
#include "cdmp_network_service.h"
namespace eerie_leap::subsys::cdmp::services {

namespace threading = eerie_leap::subsys::threading;

using threading::WorkQueueThread;
using threading::WorkQueueTaskResult;

class CdmpHeartbeatService final : public CdmpCanbusServiceBase {
private:
    std::shared_ptr<WorkQueueThread> work_queue_thread_;
    std::shared_ptr<CdmpNetworkService> network_service_;
    atomic_t is_running_ = ATOMIC_INIT(0);

    std::optional<threading::WorkQueueTask<CdmpHeartbeatService>> heartbeat_task_;
    bool is_heartbeat_task_running_ = false;
    static WorkQueueTaskResult ProcessPeriodicHeartbeat(CdmpHeartbeatService* instance);

    int canbus_handler_id_ = -1;

    // Heartbeat configuration
    uint8_t heartbeat_sequence_number_ = 0;
    bool heartbeat_enabled_ = true;

    void RegisterCanHandlers();
    void UnregisterCanHandlers();

    void OnDeviceStatusChanged(CdmpDeviceStatus old_status, CdmpDeviceStatus new_status) override;

    void StartHeartbeatTask();
    void SendHeartbeat();
    void ProcessFrame(std::span<const uint8_t> frame_data);

public:
    CdmpHeartbeatService(
        std::shared_ptr<CdmpCanIdManager> can_id_manager,
        std::shared_ptr<CdmpDevice> device,
        std::shared_ptr<WorkQueueThread> work_queue_thread,
        std::shared_ptr<CdmpNetworkService> network_service);

    ~CdmpHeartbeatService();

    void Initialize() override;
    void Start() override;
    void Stop() override;

    // Heartbeat management
    void SetHeartbeatEnabled(bool enabled);
    bool IsHeartbeatEnabled() const { return heartbeat_enabled_; }

    // Diagnostics
    void PrintHeartbeatStatus() const;
};

} // namespace eerie_leap::subsys::cdmp::services
