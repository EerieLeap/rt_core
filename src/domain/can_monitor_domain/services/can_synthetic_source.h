#pragma once

// Built only with EERIE_LEAP_CAN_MONITOR_SYNTHETIC (and its .cpp with it).
#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_SYNTHETIC

    #include <atomic>
    #include <cstdint>
    #include <memory>

    #include "domain/can_monitor_domain/models/can_monitor_types.h"
    #include "subsys/threading/i_thread.h"
    #include "subsys/threading/thread.h"

    #include "can_monitor_session.h"

namespace eerie_leap::domain::can_monitor_domain::services {

using eerie_leap::domain::can_monitor_domain::models::CanSyntheticParameters;
using eerie_leap::subsys::threading::IThread;
using eerie_leap::subsys::threading::Thread;

/**
 * @brief Writes generated frames into a session's ring through the session's own producer path, as if a
 * bus had received them, to measure the link without one (§7).
 *
 * - `ids` distinct identifiers in turn (11-bit from 0, or 29-bit from 0x18DA0000 with `ext`), the DLC
 *   `dlc`, FD with a bit rate switch when `fd`.
 * - The payload starts with the frame's number as a little-endian counter (as many bytes as the DLC
 *   gives, 8 at most) and goes on with a pattern, byte i = 0xA5 ^ i.
 * - Frames are paced by the time elapsed since the start: each wake-up writes the frames due, so the rate
 *   holds however late a wake-up is. Frame 0 is written at the start. Each frame carries NowUs().
 *
 * The thread is the session's only producer while it runs, so the session must have no tap set.
 */
class CanSyntheticSource final : public IThread {
public:
    /// The thread builds a frame and writes a record; the stack is for the unwinder too.
    static constexpr int THREAD_STACK_SIZE = 8192;
    static constexpr int THREAD_PRIORITY = 5;
    /// Shortest and longest sleep between wake-ups.
    static constexpr int32_t MIN_SLEEP_US = 1000;
    static constexpr int32_t MAX_SLEEP_US = 50000;

private:
    CanMonitorSession& session_;
    CanSyntheticParameters parameters_;
    std::unique_ptr<Thread> thread_;
    std::atomic<bool> is_stopping_{ false };
    std::atomic<uint32_t> generated_{ 0 };

    void ThreadEntry() override;
    void WriteFrame(uint64_t number) const;

public:
    CanSyntheticSource(CanMonitorSession& session, const CanSyntheticParameters& parameters);
    ~CanSyntheticSource() override;

    CanSyntheticSource(const CanSyntheticSource&) = delete;
    CanSyntheticSource& operator=(const CanSyntheticSource&) = delete;
    CanSyntheticSource(CanSyntheticSource&&) = delete;
    CanSyntheticSource& operator=(CanSyntheticSource&&) = delete;

    /** @brief The identifier of frame @p number. */
    [[nodiscard]] static uint32_t IdOf(const CanSyntheticParameters& parameters, uint64_t number);
    /** @brief Byte @p index of the payload of frame @p number. */
    [[nodiscard]] static uint8_t PayloadByte(uint64_t number, size_t index);

    bool Start();
    /** @brief Stops the thread and waits for it; the session has no producer afterwards. */
    void Stop();

    /** @brief Frames written so far (wraps at 2^32). */
    [[nodiscard]] uint32_t GetGenerated() const {
        return generated_.load(std::memory_order_relaxed);
    }
};

} // namespace eerie_leap::domain::can_monitor_domain::services

#endif // CONFIG_EERIE_LEAP_CAN_MONITOR_SYNTHETIC
