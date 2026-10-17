#pragma once

#include <array>
#include <functional>
#include <map>
#include <unordered_map>
#include <vector>
#include <memory>
#include <span>
#include <optional>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "subsys/threading/work_queue_thread.h"
#include "subsys/cdmp/models/cdmp_device_info.h"

#include "cdmp_canbus_service_base.h"


namespace eerie_leap::subsys::cdmp::services {

namespace threading = eerie_leap::subsys::threading;

using threading::WorkQueueThread;
using threading::WorkQueueTaskResult;
using eerie_leap::subsys::cdmp::utilities::CdmpConstants;
using eerie_leap::subsys::cdmp::utilities::CdmpIdClaimResult;
using eerie_leap::subsys::cdmp::models::CdmpDeviceInfo;
using eerie_leap::subsys::cdmp::models::CdmpDiscoveryResponseMessage;
using eerie_leap::subsys::cdmp::models::CdmpHeartbeatMessage;
using eerie_leap::subsys::cdmp::models::CdmpDiscoveryRequestMessage;
using eerie_leap::subsys::cdmp::models::CdmpIdClaimRequestMessage;

/** @brief A change in the table of other devices. */
enum class CdmpDeviceEvent : uint8_t {
    JOINED,    ///< Added to the table: a discovery response, or the first heartbeat after its ID claim.
    RESTARTED, ///< Claimed its ID again with its UID: it rebooted, or its COM service restarted. It stays in the table.
    LEFT,      ///< Removed from the table, normally after the heartbeat timeout.
};

/**
 * @brief Discovery, ID claim and the table of other devices on the CDMP network.
 *
 * Every unit records an ID claim as pending and adds the claimant on its first heartbeat, which
 * proves the claim went through; answering the claim stays with the lowest ID and the ID's owner.
 * A pending claim without a heartbeat expires after CdmpConstants::HEARTBEAT_TIMEOUT_MS. An online
 * unit that hears a heartbeat from a device it does not know, for example one dropped after the
 * heartbeat timeout while its cable was unplugged, asks for it again with a discovery request.
 *
 * Discovery and the ID claim run one attempt per task step, so the responses they wait for are
 * processed in between. The device table is updated on the CDMP work queue; its queries may be
 * called from any thread.
 */
class CdmpNetworkService final : public CdmpCanbusServiceBase {
public:
    /** @brief Called with the event and the device as the table knows it. */
    using DeviceEventHandler = std::function<void(CdmpDeviceEvent event, const CdmpDeviceInfo& device)>;
    /** @brief Called with a heartbeat of a device in the table, after the table is updated. */
    using HeartbeatHandler = std::function<void(const CdmpHeartbeatMessage& heartbeat)>;

private:
    struct PendingIdClaim {
        uint8_t device_id = CdmpDevice::DEVICE_ID_UNASSIGNED; // Unassigned marks a free slot.
        uint32_t uid = 0;
        int64_t claimed_at_ms = 0;
    };

    std::shared_ptr<WorkQueueThread> work_queue_thread_;

    std::optional<threading::WorkQueueTask<CdmpNetworkService>> validation_task_;
    bool is_validation_task_running_ = false;
    static WorkQueueTaskResult ProcessPeriodicValidation(CdmpNetworkService* instance);

    std::optional<threading::WorkQueueTask<CdmpNetworkService>> discovery_task_;
    static WorkQueueTaskResult ProcessDiscoveryAttempt(CdmpNetworkService* instance);

    std::optional<threading::WorkQueueTask<CdmpNetworkService>> id_claim_task_;
    static WorkQueueTaskResult ProcessIdClaimAttempt(CdmpNetworkService* instance);

    int canbus_handler_id_ = -1;
    // Incremented by every stop, so work queued for an earlier session is dropped.
    atomic_t session_ = ATOMIC_INIT(0);

    // Written on the CDMP work queue, read from any thread.
    mutable k_mutex devices_lock_;
    std::unordered_map<uint8_t, std::unique_ptr<CdmpDevice>> network_devices_;
    // Claims waiting for their first heartbeat, under devices_lock_.
    std::array<PendingIdClaim, CdmpConstants::PENDING_ID_CLAIMS_MAX> pending_id_claims_{};

    // Handlers run without devices_lock_, so they may query the table.
    mutable k_mutex handlers_lock_;
    int next_handler_id_ = 0;
    std::map<int, DeviceEventHandler> device_event_handlers_;
    std::map<int, HeartbeatHandler> heartbeat_handlers_;

    bool auto_discovery_enabled_ = true;

    // Discovery and ID claim progress, on the CDMP work queue.
    size_t discovery_attempt_ = 0;
    int discovery_backoff_resets_ = 0;
    bool discovery_response_received_ = false;
    bool lower_uid_discovering_ = false;
    uint8_t claiming_device_id_ = CdmpDevice::DEVICE_ID_UNASSIGNED;
    int id_claim_attempt_ = 0;
    std::optional<CdmpIdClaimResult> id_claim_result_ = std::nullopt;
    std::optional<int64_t> rediscovery_requested_at_ms_ = std::nullopt;

    /** @brief Runs @p work on the CDMP work queue unless the service stops before it gets there. */
    template<typename TWork>
    void RunInSession(TWork&& work) {
        const atomic_val_t session = atomic_get(&session_);

        work_queue_thread_->Run([this, session, work = std::forward<TWork>(work)]() {
            if(atomic_get(&session_) == session)
                work();
        });
    }

    void RegisterCanHandlers();
    void UnregisterCanHandlers();

    void OnDeviceStatusChanged(CdmpDeviceStatus old_status, CdmpDeviceStatus new_status) override;

    void StartValidationTask();

    // On the CDMP work queue.
    void StartInitialization();
    WorkQueueTaskResult RunDiscoveryAttempt();
    void StartIdClaim();
    WorkQueueTaskResult RunIdClaimAttempt();
    void CompleteIdClaim();

    void ProcessDiscoveryRequestFrame(std::span<const uint8_t> frame_data);
    void ProcessDiscoveryResponseFrame(std::span<const uint8_t> frame_data);
    void SendDiscoveryRequest();
    void SendDiscoveryResponse();
    /** @brief Asks every device for its identity, at most once per heartbeat interval. */
    void RequestRediscovery(uint8_t unknown_device_id);

    void SendIdClaim();
    void ProcessIdClaimRequestFrame(std::span<const uint8_t> frame_data);
    void ProcessIdClaimResponseFrame(std::span<const uint8_t> frame_data);
    void RecordIdClaim(const CdmpIdClaimRequestMessage& claim);

    // Under devices_lock_.
    PendingIdClaim* FindPendingIdClaim(uint8_t device_id);
    void StorePendingIdClaim(const CdmpIdClaimRequestMessage& claim);
    void ExpirePendingIdClaims();
    std::optional<uint8_t> FindDeviceIdByUid(uint32_t uid) const;

    /**
     * @brief Adds the device or marks it alive, and stores its features (0 included: a unit that
     *        clears every feature says so).
     * @return Whether the device was added rather than updated.
     */
    bool AddOrUpdateDevice(uint8_t device_id, uint32_t uid, uint16_t features);
    void UpdateDeviceFromDiscovery(const CdmpDiscoveryResponseMessage& discovery);
    /** @brief A valid device ID other than this unit's own while it is online. */
    bool IsPeerDeviceId(uint8_t device_id) const;

    void NotifyDeviceEvent(CdmpDeviceEvent event, const CdmpDeviceInfo& device) const;
    void NotifyHeartbeat(const CdmpHeartbeatMessage& heartbeat) const;

    void RemoveOfflineDevices();
    void UpdateNetworkDevices();
    void UpdateLowestIdOnNetwork();
    void UpdateStaggeredMessageDelay();
    /** @brief The lowest ID above @p after that is neither in the table nor claimed by a pending claim. */
    uint8_t GetLowestAvailableId(uint8_t after = 0);

    bool DoInitialize() override;
    bool DoStart() override;
    bool DoStop() override;

public:
    CdmpNetworkService(
        std::shared_ptr<CdmpCanIdManager> can_id_manager,
        std::shared_ptr<CdmpDevice> device,
        std::shared_ptr<WorkQueueThread> work_queue_thread);

    ~CdmpNetworkService();

    void ProcessFrame(std::span<const uint8_t> frame_data);

    // Network device management
    /**
     * @brief Refreshes a device in the table, or adds a pending claimant. Any other sender makes an
     * online unit request discovery, whose response adds it.
     */
    void UpdateDeviceFromHeartbeat(const CdmpHeartbeatMessage& heartbeat);
    /** @brief Removes a device and reports CdmpDeviceEvent::LEFT. */
    void RemoveDevice(uint8_t device_id);
    /** @brief Forgets every device and pending claim without events; the unit's own status covers it. */
    void ClearAllDevices();

    /**
     * @brief Reports devices joining, restarting and leaving the table.
     *
     * The handler runs on the thread that processes the frame or the validation, which is the CDMP
     * work queue. It must not block, and must not register or unregister handlers.
     *
     * @return A handler ID for UnregisterDeviceEventHandler().
     */
    int RegisterDeviceEventHandler(DeviceEventHandler handler);
    void UnregisterDeviceEventHandler(int handler_id);

    /**
     * @brief Passes on each heartbeat from a device in the table, after any JOINED event it causes.
     *
     * Runs on the CDMP work queue under the same rules as the device event handler.
     *
     * @return A handler ID for UnregisterHeartbeatHandler().
     */
    int RegisterHeartbeatHandler(HeartbeatHandler handler);
    void UnregisterHeartbeatHandler(int handler_id);

    // Device queries
    std::vector<uint8_t> GetOnlineDeviceIds() const;
    std::vector<uint8_t> GetAllDeviceIds() const;
    size_t GetDeviceCount() const;

    /**
     * @brief Copies the other devices, ordered by device ID, into a caller-owned buffer.
     * @return The number of devices copied, at most @p devices.size().
     */
    size_t GetNetworkDevices(std::span<CdmpDeviceInfo> devices) const;

    // Discovery management
    void SetAutoDiscovery(bool enabled);
    bool IsAutoDiscoveryEnabled() const { return auto_discovery_enabled_; }
    /**
     * @brief Removes the devices past the heartbeat timeout (CdmpDeviceEvent::LEFT) and updates the
     * lowest ID and the stagger. The validation task runs it every NETWORK_VALIDATION_INTERVAL_MS.
     */
    void ProcessPeriodicTasks();

    // Network status
    bool IsDeviceOnline(uint8_t device_id) const;

    // Diagnostics
    void PrintNetworkStatus() const;
};

} // namespace eerie_leap::subsys::cdmp::services
