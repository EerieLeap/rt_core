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

#include "can_error_counters.h"

#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_DELTA
    #include "domain/can_monitor_domain/utilities/can_delta_table.h"
#endif

namespace eerie_leap::domain::can_monitor_domain::services {

using eerie_leap::domain::can_monitor_domain::models::CanbusType;
using eerie_leap::domain::can_monitor_domain::models::CanMonitorMode;
using eerie_leap::domain::can_monitor_domain::models::CanUnitFilter;
using eerie_leap::domain::can_monitor_domain::utilities::ByteRing;
using eerie_leap::domain::can_monitor_domain::utilities::CanCounterWidener;
using eerie_leap::domain::can_monitor_domain::utilities::CanRecordFrame;
using eerie_leap::domain::can_monitor_domain::utilities::CanRecordWriter;
using eerie_leap::domain::can_monitor_domain::utilities::CanStatistics;
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

/** @brief What a session is opened with. */
struct CanMonitorSessionParameters {
    uint32_t token = 0;
    uint8_t port = 0;
    CanMonitorMode mode = CanMonitorMode::TAP;
    CanbusType type = CanbusType::NONE;
    /// The bitrates the session's bus runs at; 0 while a bitrate is being detected, or for no bus.
    uint32_t bitrate = 0;
    uint32_t data_bitrate = 0;
    /// NowUs() at OPEN: the time of the record before the first.
    uint64_t start_time_us = 0;
    /// Changed-bytes records; ignored in a build without EERIE_LEAP_CAN_MONITOR_DELTA.
    bool is_delta = false;
    /// The controller's counts; none for a session with no bus.
    std::shared_ptr<ICanErrorCounters> counters;
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
 * - Any thread: SetFilters() (a spinlock the producer takes too) and Totals() (a mutex the producer never
 *   takes).
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
#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_DELTA
    std::unique_ptr<utilities::CanDeltaTable> delta_;
#endif
    CanRecordWriter writer_;
    std::shared_ptr<ICanErrorCounters> counters_;

    // The unit filters. The spinlock is the only thing the producer and another thread share here.
    k_spinlock filter_lock_{};
    std::array<CanUnitFilter, CONFIG_EERIE_LEAP_CAN_MONITOR_MAX_FILTERS> filters_{};
    size_t filter_count_ = 0;

    // Counters the sender reads; they wrap at 2^32 (CanCounterWidener).
    std::atomic<uint32_t> frames_received_{ 0 };
    std::atomic<uint32_t> frames_sent_{ 0 };
    std::atomic<uint32_t> busy_us_{ 0 };
    std::atomic<uint32_t> filtered_{ 0 };

    // Totals since OPEN, widened from the counters above and the controller's. Taken by the threads that
    // make statistics, never by the producer.
    k_mutex totals_lock_{};
    CanCounterWidener received_total_;
    CanCounterWidener sent_total_;
    CanCounterWidener busy_total_;
    CanCounterWidener filtered_total_;
    std::array<CanCounterWidener, utilities::CAN_GAP_CAUSE_COUNT> lost_totals_{};
    std::array<CanCounterWidener, 5> error_totals_{};
    uint64_t discarded_ = 0;
    // The controller's counts when the session started; only the changes since count.
    CanErrorCounts error_baseline_;

    // Producer only. The bus is null for a synthetic session.
    Canbus* bus_ = nullptr;
    uint32_t nominal_bitrate_;
    uint32_t data_bitrate_;
    uint32_t last_reported_bitrate_;
    uint64_t busy_remainder_ns_ = 0;
    uint32_t last_rx_dropped_ = 0;
    uint32_t last_echo_lost_ = 0;
    uint32_t last_overruns_ = 0;

    CanMonitorSession(
        const CanMonitorSessionParameters& parameters,
        CanMonitorWakeSignal& wake,
        std::unique_ptr<ByteRing> ring
    );

    /// The table the writer encodes changed bytes with; none without the option or without OPEN's `delta`.
    utilities::CanDeltaTable* GetDeltaTable();

    bool PassesFilter(uint32_t id, bool ext);
    void CollectLosses();
    void AddBusTime(const CanRecordFrame& frame);
    void WakeSender(size_t size_before);

public:
    /**
     * @return nullptr when the ring (EERIE_LEAP_CAN_MONITOR_RING_SIZE bytes from @p memory) or, with
     * OPEN's `delta`, the changed-bytes table can't be had.
     */
    static std::unique_ptr<CanMonitorSession> Create(
        const CanMonitorSessionParameters& parameters,
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
     * @brief Takes the bus's counts as the baseline for the losses and errors the session reports.
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

    /**
     * @brief Everything counted since OPEN: frames, bus time, the controller's error counts and the losses
     * at each stage, widened to 64 bits. Callable from the sender and the service; the producer never waits for it.
     */
    [[nodiscard]] CanStatistics Totals();

    /** @brief Frames the sender discarded after the ring gave them up (a record too large, a refused notification). */
    void AddDiscarded(uint64_t frames);
};

} // namespace eerie_leap::domain::can_monitor_domain::services
