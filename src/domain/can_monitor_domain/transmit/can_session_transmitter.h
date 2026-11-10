#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include <zephyr/kernel.h>

#include "domain/can_monitor_domain/models/can_monitor_types.h"
#include "domain/can_monitor_domain/services/i_can_bus_state_listener.h"
#include "domain/can_monitor_domain/utilities/can_record.h"
#include "domain/canbus_domain/models/canbus_com_configuration.h"
#include "subsys/canbus/can_frame.h"
#include "subsys/canbus/canbus.h"

#include "can_replay_buffer.h"
#include "can_reserved_ids.h"
#include "can_tx_limiter.h"

namespace eerie_leap::domain::can_monitor_domain::transmit {

/**
 * @brief What a session may transmit: ARM, SEND, PERIODIC frames and REPLAY, under the cap and the reserved IDs.
 *
 * - **Armed.** Nothing is sent before ARM. Arming takes the session's bitrates and the reserved IDs of the
 *   stored COM configuration for the port; a limiter (CanTxLimiter) caps the bus time the session transmits.
 *   Disarming (ARM off, the end of the session, bus-off) stops the periodic frames and the replay and reports
 *   a tx event: disarmed or bus-off, with the number of frames that were not sent (the frames still due of the
 *   periodic slots that have a count, and the replay's frames left).
 * - **The cap.** SEND goes through it in order and stops at the first frame over it; a replay frame over it
 *   waits; a periodic frame over it is skipped and counted. Skipped frames (those, and the replay's frames with
 *   a reserved ID or that the bus refused) are reported as tx event 3, at most once a second: the first one
 *   at once, a later one when a second has passed since the last report, even if no further frame is skipped.
 * - **Threads.** The control calls come from one thread at a time (the service's lock). The periodic frames and
 *   the replay run as delayable work on the work queue the transmitter is given; they and the control calls
 *   share one mutex, which is never held while waiting for the queue. Bus-off arrives from the bus's receive
 *   thread through OnBusState(), which only sets a flag and submits work.
 * - **Events** go to the sink, which queues them for the sender: this class never writes the session's ring.
 */
class CanSessionTransmitter final : public services::ICanBusStateListener {
public:
    using EventSink = std::function<void(utilities::CanTxEvent event, uint64_t count)>;

    /// Frames in one SEND.
    static constexpr size_t MAX_SEND_FRAMES = 16;
    /// The most often skipped frames are reported.
    static constexpr int64_t SKIP_REPORT_PERIOD_US = 1'000'000;

private:
    // The work items refer back to the transmitter, and a periodic one to its slot. Standard layout, so
    // that CONTAINER_OF() can find it from the work item.
    struct Work {
        k_work_delayable work{};
        CanSessionTransmitter* owner = nullptr;
        void* slot = nullptr;
    };

    struct PeriodicSlot {
        Work work;
        uint32_t slot = 0;
        eerie_leap::subsys::canbus::CanFrame frame;
        uint64_t period_us = 0;
        bool has_count = false;
        uint32_t remaining = 0;
        bool is_active = true;
        uint64_t due_us = 0;
    };

    eerie_leap::subsys::canbus::Canbus& bus_;
    const uint8_t port_;
    k_work_q* const queue_;
    EventSink sink_;

    k_mutex lock_{};
    bool is_armed_ = false;
    bool is_fd_channel_ = false;
    CanTxLimiter limiter_;
    CanReservedIds reserved_;
    std::vector<std::unique_ptr<PeriodicSlot>> slots_;

    std::unique_ptr<CanReplayBuffer> replay_;
    bool is_replay_running_ = false;
    uint32_t reported_underruns_ = 0;
    Work replay_work_;

    uint64_t skipped_ = 0;
    uint64_t last_skip_report_us_ = 0;
    bool has_reported_skips_ = false;

    std::atomic<bool> is_bus_off_{ false };
    Work bus_off_work_;
    // Reports skipped frames that were noted too soon after the last report to go out at once.
    Work skip_work_;

    static void PeriodicHandler(k_work* work);
    static void ReplayHandler(k_work* work);
    static void BusOffHandler(k_work* work);
    static void SkipHandler(k_work* work);

    [[nodiscard]] std::expected<eerie_leap::subsys::canbus::CanFrame, models::CanMonitorError>
    BuildFrame(const models::CanMonitorTxFrame& frame) const;
    [[nodiscard]] std::expected<void, models::CanMonitorError> CheckReady() const;
    void Disarm(utilities::CanTxEvent reason);
    void StopWork();
    void StopReplay();
    void NoteSkipped(uint64_t now_us, uint64_t count);
    void ReportSkipped(uint64_t now_us);
    void RunSkipReport();
    void RunPeriodic(PeriodicSlot& slot);
    void RunReplay();
    void ScheduleReplay(k_timeout_t delay);

public:
    /**
     * @param bus The bus the session sends on; it outlives the transmitter.
     * @param port The session's port, for the reserved IDs.
     * @param queue The work queue the periodic frames and the replay run on.
     * @param sink Where tx events go.
     */
    CanSessionTransmitter(eerie_leap::subsys::canbus::Canbus& bus, uint8_t port, k_work_q* queue, EventSink sink);
    ~CanSessionTransmitter() override;

    CanSessionTransmitter(const CanSessionTransmitter&) = delete;
    CanSessionTransmitter& operator=(const CanSessionTransmitter&) = delete;
    CanSessionTransmitter(CanSessionTransmitter&&) = delete;
    CanSessionTransmitter& operator=(CanSessionTransmitter&&) = delete;

    /**
     * @brief Arms or disarms.
     * @param com The stored COM configuration, for the reserved IDs of the session's port.
     * @return BUS_OFF while the bus is off, BAD_BITRATE while the bus has no bitrate (it is still detecting it).
     */
    std::expected<models::CanMonitorArmState, models::CanMonitorError>
    Arm(bool on, const eerie_leap::domain::canbus_domain::models::CanbusComConfiguration& com);

    /**
     * @brief Sends the frames in order, through the cap.
     *
     * Nothing is sent when one of the frames is refused: RESERVED for a reserved ID, INVALID for a frame the
     * bus cannot carry or more than MAX_SEND_FRAMES. Then each frame goes if the cap allows and the bus takes
     * it; the first that doesn't ends the request.
     *
     * @return The count sent and not sent; NOT_ARMED, BUS_OFF; RATE when the cap let nothing through.
     */
    std::expected<models::CanMonitorSendResult, models::CanMonitorError>
    Send(std::span<const models::CanMonitorTxFrame> frames);

    /**
     * @brief Replaces the periodic frames.
     *
     * RESERVED for a reserved ID; INVALID for a frame the bus cannot carry, a repeated slot, a period of 0 or
     * more slots than EERIE_LEAP_CAN_MONITOR_PERIODIC_SLOTS. An empty list stops them all.
     */
    std::expected<void, models::CanMonitorError> SetPeriodic(std::span<const models::CanMonitorPeriodic> list);

    /**
     * @brief START, CHUNK, STOP or FINISH of a replay.
     * @return The room the replay buffer has for a chunk.
     *   - NOT_ARMED (STOP excepted), BUS_OFF, NO_MEMORY (START, no buffer), INVALID (a speed out of range, a
     *     malformed chunk);
     *   - BAD_SEQ for a chunk or FINISH with no replay started, or a chunk out of order; NO_ROOM for a chunk
     *     larger than the room.
     */
    std::expected<uint32_t, models::CanMonitorError> Replay(const models::CanMonitorReplayRequest& request);

    /** @brief Disarms (reporting "disarmed" if it was armed) and stops all the work. Last call before destruction. */
    void Shutdown();

    /** @brief From the receive thread: bus-off flags the transmitter and submits the work that disarms it. */
    void OnBusState(can_state state) override;

    [[nodiscard]] bool IsArmed();
};

} // namespace eerie_leap::domain::can_monitor_domain::transmit
