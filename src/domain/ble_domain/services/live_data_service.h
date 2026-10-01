#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include <zephyr/kernel.h>

#include "subsys/bluetooth/i_ble_notifier.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "domain/ble_domain/utilities/live_data_encoder.h"

namespace eerie_leap::domain::ble_domain::services {

using eerie_leap::subsys::bluetooth::IBleNotifier;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;
using eerie_leap::domain::ble_domain::utilities::LiveDataEncoder;

/**
 * @brief Samples the latest values of the subscribed sensors and notifies them on `live`.
 *
 * Samples run on the system work queue. At most one notification is in flight: a sample that finds
 * one pending, or no free buffer, is dropped rather than queued. The sequence number counts
 * samples, so every drop leaves a gap the client can see.
 */
class LiveDataService {
public:
    enum class SubscribeResult : uint8_t {
        OK,
        NOT_LISTENING, ///< The central has not enabled `live` notifications.
        TOO_MANY,      ///< More sensors than GetCapacity().
    };

    /// Fills one notification at an ATT MTU of 247.
    static constexpr size_t MAX_SENSORS = 47;
    static constexpr uint32_t MIN_PERIOD_MS = 50;

private:
    struct SampleWork {
        k_work_delayable work;
        LiveDataService* service;
    };

    std::shared_ptr<IBleNotifier> notifier_;
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame_;

    // Guards the subscription and the buffers between the subscriber and the sample work.
    mutable k_mutex lock_{};
    std::array<uint32_t, MAX_SENSORS> sensor_id_hashes_{};
    size_t sensor_count_ = 0;
    uint32_t period_ms_ = MIN_PERIOD_MS;
    uint8_t sequence_ = 0;
    std::array<std::optional<float>, MAX_SENSORS> values_{};
    std::array<uint8_t, LiveDataEncoder::GetMaxSize(MAX_SENSORS)> buffer_{};

    SampleWork sample_work_{};
    std::atomic<bool> is_in_flight_ = false;
    std::atomic<uint32_t> dropped_ = 0;

    static void SampleWorkHandler(k_work* work);
    void Sample();

public:
    LiveDataService(std::shared_ptr<IBleNotifier> notifier, std::shared_ptr<SensorReadingsFrame> sensor_readings_frame);
    ~LiveDataService();

    LiveDataService(const LiveDataService&) = delete;
    LiveDataService& operator=(const LiveDataService&) = delete;

    /** @brief Sensors that fit into one notification now; 0 while the central is not subscribed to `live`. */
    [[nodiscard]] size_t GetCapacity() const;

    /** @brief The sampling period used for a requested @p period_ms. */
    static constexpr uint32_t ClampPeriod(uint32_t period_ms) { return std::max(period_ms, MIN_PERIOD_MS); }

    /**
     * @brief Replaces the subscription and samples right away.
     * @param sensor_id_hashes Hashes of the configured sensor IDs; notifications refer to them by position.
     */
    SubscribeResult Subscribe(std::span<const uint32_t> sensor_id_hashes, uint32_t period_ms);
    /** @brief Stops sampling, e.g. on request or when the central disconnects. */
    void Unsubscribe();
    /** @brief A `live` notification left. */
    void OnNotificationSent();

    [[nodiscard]] bool IsSubscribed() const;
    /** @brief Samples not notified: one still in flight, no free buffer, or no subscribed central. */
    [[nodiscard]] uint32_t GetDroppedCount() const { return dropped_; }
};

} // namespace eerie_leap::domain::ble_domain::services
