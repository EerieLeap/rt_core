#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <zephyr/kernel.h>

#include "domain/can_monitor_domain/utilities/can_record.h"
#include "subsys/threading/i_thread.h"
#include "subsys/threading/thread.h"

#include "can_monitor_link.h"
#include "can_monitor_session.h"

namespace eerie_leap::domain::can_monitor_domain::services {

using eerie_leap::domain::can_monitor_domain::utilities::CanCounterWidener;
using eerie_leap::domain::can_monitor_domain::utilities::CanEndReason;
using eerie_leap::domain::can_monitor_domain::utilities::CanStatistics;
using eerie_leap::subsys::threading::IThread;
using eerie_leap::subsys::threading::Thread;

/**
 * @brief The one thread that cuts every session's ring into `bus` notifications (§4.4).
 *
 * - A notification is the 13-byte header, then the sender's own records (statistics, with dt 0), then
 *   whole ring records. `base_us` is the time of the record before the first, kept as a running time per
 *   session that each consumed ring record's dt advances; `seq` counts the notifications sent.
 * - It sends when a full notification waits, EERIE_LEAP_CAN_MONITOR_FLUSH_MS after it first saw records
 *   wait, or when the session is ending.
 * - At most EERIE_LEAP_CAN_MONITOR_IN_FLIGHT notifications, over all sessions, await OnNotificationSent().
 * - A notify refused with -ENOMEM keeps the built notification (the ring is consumed only after a
 *   successful notify) and tries again after the next completion, or after a few ms; `seq` never skips.
 *   Any other error drops the notification (its frames count as lost, `seq` advances).
 * - While link.is_smp_busy() holds, nothing is sent.
 * - Each second a session gets a statistics record with the values since the previous one.
 * - A session that ends drains its ring when the link is up, then gets a final statistics record and the
 *   end record; with no link it is freed at once.
 *
 * Locks: slots_lock_ guards the session list between the sender thread and Attach/End; it is taken
 * after the service's own lock, never before. The tap never touches it.
 */
class CanMonitorSender final : public IThread {
public:
    /// The notification buffer; a notification is also cut to what the link takes.
    static constexpr size_t MAX_NOTIFICATION_SIZE = 512;
    static constexpr int64_t STATISTICS_PERIOD_MS = 1000;
    /// How often a stream held back by SMP looks again.
    static constexpr int64_t SMP_RECHECK_MS = 5;
    /// A refused notify is tried again after this long when none of ours is in flight.
    static constexpr int64_t NO_BUFFER_RETRY_MS = 5;
    /// The longest a session waits for the sender to look at the link while there is nothing to do.
    static constexpr int64_t IDLE_POLL_MS = 50;
    /// A session that ends is freed after this long even if its ring never drained.
    static constexpr int64_t DRAIN_TIMEOUT_MS = 5000;

private:
    struct Slot {
        std::unique_ptr<CanMonitorSession> session;
        uint16_t sequence = 0;
        /// The time of the record before the next one the ring gives.
        uint64_t running_us = 0;

        CanCounterWidener received;
        CanCounterWidener sent;
        CanCounterWidener busy;
        CanCounterWidener filtered;
        std::array<CanCounterWidener, utilities::CAN_GAP_CAUSE_COUNT> lost{};
        /// Frames the sender itself discarded (a record too large, a notification refused).
        uint64_t sender_lost = 0;
        /// The totals at the last statistics record.
        CanStatistics reported;
        int64_t next_statistics_ms = 0;
        std::array<uint8_t, utilities::CAN_MAX_RECORD_SIZE> statistics_record{};
        size_t statistics_size = 0;

        /// -1 while nothing waits.
        int64_t first_waiting_ms = -1;
        bool is_ending = false;
        /// The final statistics record has been made (staged or sent).
        bool is_final_staged = false;
        CanEndReason end_reason = CanEndReason::CLOSED;
        int64_t drain_deadline_ms = 0;
        bool is_done = false;
    };

    /// A notification built and not sent yet; its ring bytes are still in the ring.
    struct Built {
        bool is_valid = false;
        Slot* slot = nullptr;
        std::array<uint8_t, MAX_NOTIFICATION_SIZE> data{};
        size_t size = 0;
        size_t ring_offset = 0;
        size_t ring_bytes = 0;
        uint64_t dt_sum_us = 0;
        /// Carries the end record.
        bool is_final = false;
    };

    CanMonitorLink link_;
    CanMonitorWakeSignal wake_;
    std::unique_ptr<Thread> thread_;
    std::atomic<bool> is_stopping_{ false };

    k_mutex slots_lock_{};
    std::vector<std::unique_ptr<Slot>> slots_;
    size_t round_robin_ = 0;

    std::atomic<uint32_t> in_flight_{ 0 };
    std::atomic<bool> retry_allowed_{ false };
    std::atomic<bool> was_disconnected_{ false };

    // Sender thread only.
    Built built_;
    int64_t retry_at_ms_ = 0;
    std::array<uint8_t, MAX_NOTIFICATION_SIZE + utilities::CAN_MAX_RECORD_SIZE> scratch_{};

    void ThreadEntry() override;
    int64_t RunPass();
    void HandleEvents();
    [[nodiscard]] size_t GetNotifyMax() const;
    [[nodiscard]] bool IsSmpBusy() const;
    [[nodiscard]] bool CanSend(int64_t now_ms, size_t notify_max, int64_t& next_ms) const;

    bool Serve(Slot& slot, int64_t now_ms, size_t notify_max, bool can_send, int64_t& next_ms);
    void StageStatistics(Slot& slot, int64_t now_ms);
    [[nodiscard]] CanStatistics TakeStatistics(Slot& slot);
    bool Build(Slot& slot, size_t notify_max);
    bool Submit(int64_t now_ms);
    bool RetryBuilt(int64_t now_ms, int64_t& next_ms);
    void Commit(int64_t now_ms);
    void Drop(int64_t now_ms);
    void Sweep();
    void ReleaseInFlight();

public:
    explicit CanMonitorSender(CanMonitorLink link);
    ~CanMonitorSender() override;

    CanMonitorSender(const CanMonitorSender&) = delete;
    CanMonitorSender& operator=(const CanMonitorSender&) = delete;
    CanMonitorSender(CanMonitorSender&&) = delete;
    CanMonitorSender& operator=(CanMonitorSender&&) = delete;

    /** @brief Creates and starts the thread; false when it can't be had. Safe to call again once started. */
    bool Start();
    /** @brief Stops the thread. Sessions that still wait are freed with the sender. */
    void Stop();
    [[nodiscard]] bool IsRunning() const;

    /** @brief What the sessions' producers wake the sender with. */
    [[nodiscard]] CanMonitorWakeSignal& GetWakeSignal() {
        return wake_;
    }

    /**
     * @brief Takes the session; the sender serves it until it is ended.
     * @throws std::bad_alloc
     */
    void Attach(std::unique_ptr<CanMonitorSession> session);

    /**
     * @brief Frees a session at once, with no end record (an OPEN that failed after Attach()).
     * The producer must be stopped.
     */
    void Discard(const CanMonitorSession* session);

    /**
     * @brief Ends a session: drains it when the link is up, with a final statistics record and the end
     * record, then frees it. The producer must be stopped.
     */
    void End(const CanMonitorSession* session, CanEndReason reason);

    /** @brief A `bus` notification left; callable from any thread. */
    void OnNotificationSent();

    /** @brief The link ended: nothing in flight will report, and sessions that end are freed undrained. */
    void OnDisconnected();

    /** @brief Notifications awaiting OnNotificationSent(). */
    [[nodiscard]] uint32_t GetInFlight() const {
        return in_flight_.load(std::memory_order_relaxed);
    }
    /** @brief Sessions the sender holds, the ones draining included. */
    [[nodiscard]] size_t GetSessionCount();
};

} // namespace eerie_leap::domain::can_monitor_domain::services
