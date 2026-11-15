#pragma once

#include <cstdint>

#include "domain/can_monitor_domain/utilities/can_record.h"
#include "subsys/canbus/can_frame.h"

// The header is pulled in by code that can be built without the transmit part.
#ifndef CONFIG_EERIE_LEAP_CAN_MONITOR_TX_SHARE_PERCENT
    #define CONFIG_EERIE_LEAP_CAN_MONITOR_TX_SHARE_PERCENT 50
#endif

#ifndef CONFIG_EERIE_LEAP_CAN_MONITOR_TX_BURST_FRAMES
    #define CONFIG_EERIE_LEAP_CAN_MONITOR_TX_BURST_FRAMES 8
#endif

namespace eerie_leap::domain::can_monitor_domain::transmit {

/**
 * @brief The cap on the bus time a session transmits: a token bucket whose tokens are bus time.
 *
 * Each frame costs its bit time (CanBitTime, without stuffing), and the bucket refills at
 * @c share_percent % of the elapsed time. The bucket holds @c burst_frames times the bit time of the
 * largest frame the channel carries: a 64-byte, 29-bit FD frame with BRS at the session's nominal and
 * data bitrates on an FD channel, an 8-byte 29-bit classical frame otherwise. So a frame of that size
 * always fits an idle bucket. An FD frame without BRS takes longer than that frame; when it is longer
 * than the whole bucket (a small burst, at an FD data bitrate well above the nominal one) it costs the whole bucket.
 *
 * Integer arithmetic only: the credit is in ns scaled by 100, so every µs adds exactly
 * @c share_percent * 1000 and nothing is lost to rounding, however long the limiter runs. Time going
 * backwards adds nothing. The caller passes the time (µs on any monotonic base) and serialises the
 * calls; nothing here waits or blocks.
 */
class CanTxLimiter {
private:
    // Credit is bus time in ns scaled by 100, so each µs adds share * 1000.
    int64_t rate_ = 0;
    int64_t capacity_ = 0;
    int64_t credit_ = 0;
    int64_t last_update_us_ = 0;
    uint32_t nominal_bitrate_ = 0;
    uint32_t data_bitrate_ = 0;

public:
    static constexpr uint8_t DEFAULT_SHARE_PERCENT = CONFIG_EERIE_LEAP_CAN_MONITOR_TX_SHARE_PERCENT;
    static constexpr uint32_t DEFAULT_BURST_FRAMES = CONFIG_EERIE_LEAP_CAN_MONITOR_TX_BURST_FRAMES;

    /** @brief A limiter that never lets a frame through. */
    CanTxLimiter() = default;

    /**
     * @param nominal_bitrate The channel's nominal bitrate, bit/s.
     * @param data_bitrate The CAN FD data bitrate; 0 means the nominal one. Ignored on a classical channel.
     * @param is_fd Whether the channel is a CAN FD one.
     * @param share_percent The share of the bus time the session may use, 1 to 100.
     * @param burst_frames The bucket's depth in largest frames, at least 1.
     * @param now_us The time the full bucket starts at.
     * @throws std::invalid_argument for a bitrate of 0, a share outside 1 to 100 or a burst of 0.
     */
    CanTxLimiter(
        uint32_t nominal_bitrate,
        uint32_t data_bitrate,
        bool is_fd,
        uint8_t share_percent,
        uint32_t burst_frames,
        int64_t now_us
    );

    /** @brief The same with the share and the burst of the build (Kconfig). */
    CanTxLimiter(uint32_t nominal_bitrate, uint32_t data_bitrate, bool is_fd, int64_t now_us) :
        CanTxLimiter(nominal_bitrate, data_bitrate, is_fd, DEFAULT_SHARE_PERCENT, DEFAULT_BURST_FRAMES, now_us) {}

    /** @brief Starts over with a full bucket at @p now_us. */
    void Reset(int64_t now_us);

    /** @brief Replaces the limiter (a new session's bitrates); strong guarantee: unchanged when it throws. */
    void Reset(
        uint32_t nominal_bitrate,
        uint32_t data_bitrate,
        bool is_fd,
        uint8_t share_percent,
        uint32_t burst_frames,
        int64_t now_us
    );

    /** @brief Adds the credit earned since the last update; time going backwards adds none. */
    void Update(int64_t now_us);

    /**
     * @brief Charges a frame if the bucket holds its cost.
     *
     * Call Update() first. A cost above the bucket's depth is charged as the whole bucket.
     *
     * @param cost_ns The frame's bit time, see CostNs().
     * @return false (and nothing charged) when the bucket holds less.
     */
    bool TryTake(uint64_t cost_ns);

    /**
     * @brief Time until a frame of @p cost_ns may be taken, counted from the last Update().
     * @return 0 if now; INT64_MAX for a limiter that never lets a frame through.
     */
    [[nodiscard]] int64_t WaitUs(uint64_t cost_ns) const;

    /** @brief The bit time of a frame at this limiter's bitrates, in ns. */
    [[nodiscard]] uint64_t CostNs(const eerie_leap::subsys::canbus::CanFrame& frame) const;
    [[nodiscard]] uint64_t CostNs(const utilities::CanRecordFrame& frame) const;

    /** @brief What the bucket holds, in ns of bus time. */
    [[nodiscard]] uint64_t CreditNs() const {
        return static_cast<uint64_t>(credit_ / 100);
    }
    /** @brief The bucket's depth in ns of bus time; 0 for a limiter that never lets a frame through. */
    [[nodiscard]] uint64_t CapacityNs() const {
        return static_cast<uint64_t>(capacity_ / 100);
    }
};

} // namespace eerie_leap::domain::can_monitor_domain::transmit
