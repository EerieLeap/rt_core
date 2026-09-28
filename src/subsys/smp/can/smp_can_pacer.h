#pragma once

#include <cstdint>

namespace eerie_leap::subsys::smp::can {

/** @brief Token bucket that limits frames to a share of the bus time. */
class SmpCanPacer {
private:
    // Credit is bus time in nanoseconds scaled by 100, so each microsecond adds share * 1000.
    int64_t rate_ = 0;
    int64_t frame_cost_ = 0;
    int64_t capacity_ = 0;
    int64_t credit_ = 0;
    int64_t last_update_us_ = 0;

public:
    /** @brief A pacer that never allows a frame. */
    SmpCanPacer() = default;

    /**
     * @param frame_time_ns Worst-case frame duration, see SmpCanFrameFormat::GetFrameTimeNs().
     * @param share_percent Share of the bus time the frames may use, 1-100.
     * @param burst_frames Bucket size: frames that may follow each other after an idle period.
     * @throws std::invalid_argument if a parameter is 0 or the share exceeds 100.
     */
    SmpCanPacer(uint32_t frame_time_ns, uint8_t share_percent, uint32_t burst_frames);

    /** @brief Starts over with a full bucket. */
    void Reset(int64_t now_us);
    /** @brief Adds the credit earned since the last update; time going backwards adds none. */
    void Update(int64_t now_us);

    /** @brief True when one frame may be sent now. */
    [[nodiscard]] bool CanSend() const { return rate_ > 0 && credit_ >= frame_cost_; }
    /** @brief Charges one frame; call only after CanSend(). */
    void OnSent();

    /** @brief Time until @p frames frames may be sent (capped at the bucket size), 0 if now. */
    [[nodiscard]] int64_t GetWaitUs(uint32_t frames = 1) const;
    /** @brief Long-run frame rate. */
    [[nodiscard]] uint32_t GetFramesPerSecond() const;
};

} // namespace eerie_leap::subsys::smp::can
