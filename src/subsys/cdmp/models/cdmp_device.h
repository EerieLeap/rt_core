#pragma once

#include <cstdint>
#include <chrono>
#include <zephyr/kernel.h>

#include "subsys/cdmp/utilities/cdmp_status_machine.h"

namespace eerie_leap::subsys::cdmp::models {

using eerie_leap::subsys::cdmp::utilities::CdmpDeviceStatus;
using eerie_leap::subsys::cdmp::utilities::CdmpHealthStatus;
using eerie_leap::subsys::cdmp::utilities::CdmpStatusMachine;

enum class CdmpDeviceType : uint8_t {
    NONE = 0,
    LOGGER = 1,
    DISPLAY = 2
};

enum class CdmpDeviceCapabilityFlags : uint32_t {
    NONE = 0,
    LOGGER_SUPPORT = 1 << 0,
    DISPLAY_SUPPORT = 1 << 1,
    DATA_STORAGE = 1 << 2
};

class CdmpDevice {
private:
    uint8_t device_id_ = DEVICE_ID_UNASSIGNED;
    uint32_t uid_ = 0;
    CdmpDeviceType device_type_ = CdmpDeviceType::NONE;
    uint8_t protocol_version_;
    CdmpHealthStatus health_status_ = CdmpHealthStatus::OK;
    uint32_t capability_flags_ = 0;
    uint64_t last_heartbeat_;
    uint8_t uptime_counter_ = 0;
    bool is_lowest_id_on_network_ = false;
    uint32_t staggered_message_delay_ = 0;

    std::shared_ptr<CdmpStatusMachine> status_machine_;

public:
    static constexpr uint8_t DEVICE_ID_UNASSIGNED = 0x00;
    static constexpr uint8_t DEVICE_ID_BROADCAST = 0xFF;

    CdmpDevice(
        uint32_t uid,
        CdmpDeviceType device_type,
        CdmpDeviceStatus status = CdmpDeviceStatus::OFFLINE);

    // Status transitions
    void StartDiscovery();
    void ClaimId();
    void GoOnline(bool force = false);
    void EnterVersionMismatch();
    void EnterError();
    void Reset();

    // Getters
    CdmpStatusMachine& GetStatusMachine() { return *status_machine_; }
    [[nodiscard]] uint8_t GetDeviceId() const { return device_id_; }
    [[nodiscard]] uint32_t GetUniqueIdentifier() const { return uid_; }
    [[nodiscard]] CdmpDeviceType GetDeviceType() const { return device_type_; }
    [[nodiscard]] uint8_t GetProtocolVersion() const { return protocol_version_; }
    [[nodiscard]] CdmpDeviceStatus GetStatus() const { return status_machine_->GetCurrentStatus(); }
    [[nodiscard]] CdmpHealthStatus GetHealthStatus() const { return health_status_; }
    [[nodiscard]] uint32_t GetCapabilityFlags() const { return capability_flags_; }
    [[nodiscard]] uint64_t GetLastHeartbeatDeltaMs() const { return k_uptime_get() - last_heartbeat_; }
    [[nodiscard]] uint8_t GetUptimeCounter() const { return uptime_counter_; }
    [[nodiscard]] bool IsLowestIdOnNetwork() const { return is_lowest_id_on_network_; }
    [[nodiscard]] uint32_t GetStaggeredMessageDelay() const { return staggered_message_delay_; }


    // Setters
    void SetDeviceId(uint8_t device_id) { device_id_ = device_id; }
    void SetHealthStatus(CdmpHealthStatus health_status) { health_status_ = health_status; }
    void SetCapabilityFlags(uint32_t flags) { capability_flags_ = flags; }
    void UpdateHeartbeat();
    void IncrementUptime() { uptime_counter_++; }
    void SetIsLowestIdOnNetwork(bool is_lowest_id_on_network) {
        is_lowest_id_on_network_ = is_lowest_id_on_network;
    }
    void SetStaggeredMessageDelay(uint32_t staggered_message_delay) {
        staggered_message_delay_ = staggered_message_delay;
    }

    // Utility methods
    bool IsOnline() const { return status_machine_->GetCurrentStatus() == CdmpDeviceStatus::ONLINE; }
    bool HasCapability(CdmpDeviceCapabilityFlags capability) const { return (capability_flags_ & static_cast<uint32_t>(capability)) != 0; }
    void SetCapability(CdmpDeviceCapabilityFlags capability, bool enabled);

    // Protocol version compatibility
    bool IsProtocolCompatible(uint8_t other_version) const;
    static uint8_t GetCurrentProtocolVersion() { return 0x10; } // Version 1.0
};

} // namespace eerie_leap::subsys::cdmp::models
