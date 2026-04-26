#pragma once

#include <unordered_map>
#include <vector>
#include <memory>
#include <span>
#include <optional>

#include "subsys/threading/work_queue_thread.h"

#include "cdmp_canbus_service_base.h"


namespace eerie_leap::subsys::cdmp::services {

using namespace eerie_leap::subsys::threading;

class CdmpNetworkService : public CdmpCanbusServiceBase {
private:
    std::shared_ptr<WorkQueueThread> work_queue_thread_;
    atomic_t is_running_ = ATOMIC_INIT(0);

    std::optional<WorkQueueTask<CdmpNetworkService>> validation_task_;
    bool is_validation_task_running_ = false;
    static WorkQueueTaskResult ProcessPeriodicValidation(CdmpNetworkService* instance);

    int canbus_handler_id_ = -1;

    std::unordered_map<uint8_t, std::unique_ptr<CdmpDevice>> network_devices_;

    // Discovery and tracking
    bool auto_discovery_enabled_ = true;
    std::vector<uint32_t> discovery_requesting_uids_;
    bool discovery_response_received_ = false;
    uint8_t claiming_device_id_ = 0;
    std::optional<CdmpIdClaimResult> id_claim_result_ = std::nullopt;

    void RegisterCanHandlers();
    void UnregisterCanHandlers();

    void OnDeviceStatusChanged(CdmpDeviceStatus old_status, CdmpDeviceStatus new_status) override;

    void StartValidationTask();

    void ProcessDiscoveryRequestFrame(std::span<const uint8_t> frame_data);
    void ProcessDiscoveryResponseFrame(std::span<const uint8_t> frame_data);
    void SendDiscoveryRequest();
    void SendDiscoveryResponse();

    void SendIdClaim();
    void ProcessIdClaimRequestFrame(std::span<const uint8_t> frame_data);
    void ProcessIdClaimResponseFrame(std::span<const uint8_t> frame_data);

    void AddOrUpdateDevice(
        uint8_t device_id,
        CdmpDeviceType device_type,
        uint32_t uid,
        uint32_t capability_flags = 0);
    void UpdateDeviceFromDiscovery(const CdmpDiscoveryResponseMessage& discovery);

    void RemoveOfflineDevices();
    void UpdateNetworkDevices();
    void UpdateLowestIdOnNetwork();
    void UpdateStaggeredMessageDelay();
    uint8_t GetLowestAvailableId(uint8_t after = 0) const;

public:
    CdmpNetworkService(
        std::shared_ptr<CdmpCanIdManager> can_id_manager,
        std::shared_ptr<CdmpDevice> device,
        std::shared_ptr<WorkQueueThread> work_queue_thread);

    ~CdmpNetworkService();

    void Initialize() override;
    void Start() override;
    void Stop() override;

    void ProcessFrame(std::span<const uint8_t> frame_data);
    void StartInitialization();

    // Network device management
    void UpdateDeviceFromHeartbeat(const CdmpHeartbeatMessage& heartbeat);
    void RemoveDevice(uint8_t device_id);
    void ClearAllDevices();

    // Device queries
    std::vector<uint8_t> GetOnlineDeviceIds() const;
    std::vector<uint8_t> GetAllDeviceIds() const;
    size_t GetDeviceCount() const { return network_devices_.size(); }
    const CdmpDevice* GetDevice(uint8_t device_id) const;
    CdmpDevice* GetDevice(uint8_t device_id); // Non-const version for modifications

    // Discovery management
    void SetAutoDiscovery(bool enabled);
    bool IsAutoDiscoveryEnabled() const { return auto_discovery_enabled_; }
    void ProcessPeriodicTasks();

    // Network status
    bool IsDeviceOnline(uint8_t device_id) const;

    // Diagnostics
    void PrintNetworkStatus() const;
};

} // namespace eerie_leap::subsys::cdmp::services
