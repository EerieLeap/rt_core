#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <zephyr/kernel.h>

#include "subsys/cdmp/utilities/cdmp_status_machine.h"

namespace eerie_leap::subsys::cdmp::models {

using eerie_leap::subsys::cdmp::utilities::CdmpDeviceStatus;
using eerie_leap::subsys::cdmp::utilities::CdmpHealthStatus;
using eerie_leap::subsys::cdmp::utilities::CdmpStatusMachine;

class CdmpDevice {
private:
    uint8_t device_id_ = DEVICE_ID_UNASSIGNED;
    uint32_t uid_ = 0;
    uint8_t protocol_version_;
    CdmpHealthStatus health_status_ = CdmpHealthStatus::OK;
    // Application feature mask (EerieLeap: ProductFeature). The CDMP layer only carries it.
    // Set by the application on any thread, read by the heartbeat and discovery services.
    std::atomic<uint16_t> features_ = 0;
    uint64_t last_heartbeat_ = 0;
    uint8_t uptime_counter_ = 0;
    bool is_lowest_id_on_network_ = false;
    uint32_t staggered_message_delay_ = 0;
    // Written by the state service on any thread, read by the heartbeat service.
    std::atomic<uint8_t> state_digest_ = 0;

    std::shared_ptr<CdmpStatusMachine> status_machine_;

public:
    static constexpr uint8_t DEVICE_ID_UNASSIGNED = 0x00;
    static constexpr uint8_t DEVICE_ID_BROADCAST = 0xFF;

    CdmpDevice(uint32_t uid, uint16_t features, CdmpDeviceStatus status = CdmpDeviceStatus::OFFLINE);

    // Status transitions
    void StartDiscovery();
    void ClaimId();
    void GoOnline(bool force = false);
    void EnterVersionMismatch();
    void EnterError();
    void Reset();

    // Getters
    CdmpStatusMachine& GetStatusMachine() {
        return *status_machine_;
    }
    [[nodiscard]] uint8_t GetDeviceId() const {
        return device_id_;
    }
    [[nodiscard]] uint32_t GetUniqueIdentifier() const {
        return uid_;
    }
    [[nodiscard]] uint8_t GetProtocolVersion() const {
        return protocol_version_;
    }
    [[nodiscard]] CdmpDeviceStatus GetStatus() const {
        return status_machine_->GetCurrentStatus();
    }
    [[nodiscard]] CdmpHealthStatus GetHealthStatus() const {
        return health_status_;
    }
    /** @brief The feature mask this device sends in heartbeats and discovery responses. Any thread. */
    [[nodiscard]] uint16_t GetFeatures() const {
        return features_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t GetLastHeartbeatDeltaMs() const {
        return k_uptime_get() - last_heartbeat_;
    }
    [[nodiscard]] uint8_t GetUptimeCounter() const {
        return uptime_counter_;
    }
    [[nodiscard]] bool IsLowestIdOnNetwork() const {
        return is_lowest_id_on_network_;
    }
    [[nodiscard]] uint32_t GetStaggeredMessageDelay() const {
        return staggered_message_delay_;
    }
    /** @brief CRC-8 over this unit's shared state records, sent in heartbeat byte 7. */
    [[nodiscard]] uint8_t GetStateDigest() const {
        return state_digest_.load(std::memory_order_relaxed);
    }

    // Setters
    void SetDeviceId(uint8_t device_id) {
        device_id_ = device_id;
    }
    void SetHealthStatus(CdmpHealthStatus health_status) {
        health_status_ = health_status;
    }
    /** @brief Replaces the feature mask; the next heartbeat carries it. Any thread. */
    void SetFeatures(uint16_t features) {
        features_.store(features, std::memory_order_relaxed);
    }
    /** @brief Sets or clears the bits of @p mask, leaving the others. Any thread. */
    void SetFeature(uint16_t mask, bool is_set) {
        if(is_set)
            features_.fetch_or(mask, std::memory_order_relaxed);
        else
            features_.fetch_and(static_cast<uint16_t>(~mask), std::memory_order_relaxed);
    }
    void UpdateHeartbeat();
    void IncrementUptime() {
        uptime_counter_++;
    }
    void SetIsLowestIdOnNetwork(bool is_lowest_id_on_network) {
        is_lowest_id_on_network_ = is_lowest_id_on_network;
    }
    void SetStaggeredMessageDelay(uint32_t staggered_message_delay) {
        staggered_message_delay_ = staggered_message_delay;
    }
    void SetStateDigest(uint8_t state_digest) {
        state_digest_.store(state_digest, std::memory_order_relaxed);
    }

    // Utility methods
    bool IsOnline() const {
        return status_machine_->GetCurrentStatus() == CdmpDeviceStatus::ONLINE;
    }

    // Protocol version compatibility
    bool IsProtocolCompatible(uint8_t other_version) const;
    static uint8_t GetCurrentProtocolVersion() {
        return 0x10;
    } // Version 1.0
};

} // namespace eerie_leap::subsys::cdmp::models
