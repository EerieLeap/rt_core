#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>

#include "domain/can_monitor_domain/models/can_monitor_types.h"
#include "domain/can_monitor_domain/utilities/byte_ring.h"
#include "domain/can_monitor_domain/utilities/can_record_writer.h"
#include "subsys/canbus/canbus.h"
#include "subsys/canbus/i_can_frame_tap.h"

namespace eerie_leap::domain::can_monitor_domain::services {

using eerie_leap::domain::can_monitor_domain::models::CanbusType;
using eerie_leap::domain::can_monitor_domain::models::CanMonitorMode;
using eerie_leap::domain::can_monitor_domain::models::CanUnitFilter;
using eerie_leap::domain::can_monitor_domain::utilities::ByteRing;
using eerie_leap::domain::can_monitor_domain::utilities::CanRecordFrame;
using eerie_leap::domain::can_monitor_domain::utilities::CanRecordWriter;
using eerie_leap::subsys::canbus::Canbus;
using eerie_leap::subsys::canbus::CanFrame;
using eerie_leap::subsys::canbus::ICanFrameTap;

/**
 * @brief How a session's producer wakes the sender. Owned by the sender, which outlives its sessions.
 */
struct CanMonitorWakeSignal {
    k_sem semaphore{};
    /// The ring size at which a full notification waits; the sender publishes it. UINT32_MAX while no
    /// notification can go.
    std::atomic<uint32_t> fill_threshold{ UINT32_MAX };

    CanMonitorWakeSignal() {
        k_sem_init(&semaphore, 0, 1);
    }

    void Wake() {
        k_sem_give(&semaphore);
    }
};

/**
 * @brief One open session: the tap of a bus (or the synthetic source's producer path), the ring its
 * records wait in, and the counters the statistics record is made of.
 *
 * Threads:
 * - Producer: the bus's receive thread through ICanFrameTap (a meta-IRQ on the gauge), or the synthetic
 *   source's thread. It never blocks, takes no k_mutex, allocates nothing and logs nothing per frame.
 *   Everything it writes is either an atomic or only it touches.
 * - Sender: reads the ring and the atomic counters.
 * - Any thread: SetFilters() (a spinlock the producer takes too).
 *
 * A session is destroyed only after its producer has stopped (the tap removed, the source joined).
 */
class CanMonitorSession final : public ICanFrameTap {
private:
    const uint32_t token_;
    const uint8_t port_;
    const CanMonitorMode mode_;
    const CanbusType type_;
    const uint64_t start_time_us_;

    CanMonitorWakeSignal& wake_;
    std::unique_ptr<ByteRing> ring_;
    CanRecordWriter writer_;

    // The unit filters. The spinlock is the only thing the producer and another thread share here.
    k_spinlock filter_lock_{};
    std::array<CanUnitFilter, CONFIG_EERIE_LEAP_CAN_MONITOR_MAX_FILTERS> filters_{};
    size_t filter_count_ = 0;

    // Counters the sender reads; they wrap at 2^32 (CanCounterWidener).
    std::atomic<uint32_t> frames_received_{ 0 };
    std::atomic<uint32_t> frames_sent_{ 0 };
    std::atomic<uint32_t> busy_us_{ 0 };
    std::atomic<uint32_t> filtered_{ 0 };

    // Producer only. The bus is null for a synthetic session.
    Canbus* bus_ = nullptr;
    const device* device_ = nullptr;
    uint32_t nominal_bitrate_;
    uint32_t data_bitrate_;
    uint32_t last_reported_bitrate_;
    uint64_t busy_remainder_ns_ = 0;
    uint32_t last_rx_dropped_ = 0;
    uint32_t last_echo_lost_ = 0;
    uint32_t last_overruns_ = 0;

    CanMonitorSession(
        uint32_t token,
        uint8_t port,
        CanMonitorMode mode,
        CanbusType type,
        uint32_t bitrate,
        uint32_t data_bitrate,
        uint64_t start_time_us,
        CanMonitorWakeSignal& wake,
        std::unique_ptr<ByteRing> ring
    );

    bool PassesFilter(uint32_t id, bool ext);
    void CollectLosses();
    void AddBusTime(const CanRecordFrame& frame);
    void WakeSender(size_t size_before);

public:
    /**
     * @param start_time_us The session's start (NowUs() at OPEN): the time of the record before the first.
     * @return nullptr when the ring (EERIE_LEAP_CAN_MONITOR_RING_SIZE bytes from @p memory) can't be had.
     */
    static std::unique_ptr<CanMonitorSession> Create(
        uint32_t token,
        uint8_t port,
        CanMonitorMode mode,
        CanbusType type,
        uint32_t bitrate,
        uint32_t data_bitrate,
        uint64_t start_time_us,
        CanMonitorWakeSignal& wake,
        std::pmr::memory_resource* memory
    );

    ~CanMonitorSession() override = default;

    CanMonitorSession(const CanMonitorSession&) = delete;
    CanMonitorSession& operator=(const CanMonitorSession&) = delete;
    CanMonitorSession(CanMonitorSession&&) = delete;
    CanMonitorSession& operator=(CanMonitorSession&&) = delete;

    [[nodiscard]] uint32_t GetToken() const {
        return token_;
    }
    [[nodiscard]] uint8_t GetPort() const {
        return port_;
    }
    [[nodiscard]] CanMonitorMode GetMode() const {
        return mode_;
    }
    [[nodiscard]] CanbusType GetType() const {
        return type_;
    }
    [[nodiscard]] uint64_t GetStartTimeUs() const {
        return start_time_us_;
    }

    /**
     * @brief Replaces the unit filters; callable from any thread while the tap runs.
     * @return false, changing nothing, when there are more than EERIE_LEAP_CAN_MONITOR_MAX_FILTERS.
     */
    bool SetFilters(std::span<const CanUnitFilter> filters);

    /**
     * @brief Takes the bus's counters as the baseline for the losses the session reports.
     *
     * Call before the tap is set, with the bus the tap will be set on. A synthetic session binds none.
     */
    void BindBus(Canbus* bus);

    // ICanFrameTap: the producer path. The synthetic source calls OnFrame() too.

    void OnFrame(const CanFrame& frame, uint64_t time_us, bool is_transmit, int tx_error) override;
    void OnState(can_state state, can_bus_err_cnt counters, uint64_t time_us) override;
    void OnFilterSwap(bool may_have_missed, uint64_t time_us) override;

    /**
     * @brief Writes the gap records for losses still waiting, as the next frame would have.
     *
     * Only once the producer has stopped (a session that ends): the sender calls it then, so that a
     * stream that overflowed just before its end still says so. False while the ring has no room.
     */
    bool FlushLossesAfterStop() {
        return writer_.FlushLosses();
    }

    // Sender thread.

    [[nodiscard]] ByteRing& GetRing() {
        return *ring_;
    }
    [[nodiscard]] const CanRecordWriter& GetWriter() const {
        return writer_;
    }
    [[nodiscard]] uint32_t GetFramesReceived() const {
        return frames_received_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint32_t GetFramesSent() const {
        return frames_sent_.load(std::memory_order_relaxed);
    }
    /** @brief Whole µs of bus time; the sub-µs rest waits in the producer. */
    [[nodiscard]] uint32_t GetBusyUs() const {
        return busy_us_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint32_t GetFiltered() const {
        return filtered_.load(std::memory_order_relaxed);
    }
};

} // namespace eerie_leap::domain::can_monitor_domain::services
