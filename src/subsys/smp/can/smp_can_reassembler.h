#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "smp_can_frame_format.h"

namespace eerie_leap::subsys::smp::can {

/** @brief Rebuilds one packet at a time from SmpCanFramer frames into a caller-owned buffer. */
class SmpCanReassembler {
public:
    /** @brief Outcome of Accept(). */
    enum class Result : uint8_t {
        IN_PROGRESS, ///< Frame accepted, packet incomplete.
        COMPLETE,    ///< GetLength() bytes of the buffer hold the packet.
        DROPPED,     ///< Sequence gap, oversize or malformed frame; the packet is discarded.
        IGNORED,     ///< Continuation frame without a packet in progress.
    };

    /// Inter-frame timeout of the CDMP specification.
    static constexpr int64_t DEFAULT_TIMEOUT_MS = 100;

private:
    std::span<uint8_t> buffer_;
    SmpCanFrameFormat format_;
    int64_t timeout_ms_;
    size_t length_ = 0;
    size_t packet_size_ = 0;  // 0 until the SMP header is complete
    int64_t last_frame_ms_ = 0;
    uint8_t next_sequence_ = 0;
    bool is_active_ = false;

    Result Drop();
    Result Append(std::span<const uint8_t> data);

public:
    /** @param timeout_ms A packet in progress is discarded after this long without a frame. */
    explicit SmpCanReassembler(int64_t timeout_ms = DEFAULT_TIMEOUT_MS);

    /** @brief Sets the buffer and the frame format for the next packet, and resets the state. */
    void Attach(std::span<uint8_t> buffer, SmpCanFrameFormat format = {});
    /** @brief Discards the packet in progress. */
    void Reset();

    [[nodiscard]] SmpCanFrameFormat GetFormat() const { return format_; }
    /** @brief True while a packet is in progress. */
    [[nodiscard]] bool IsActive() const { return is_active_; }
    /** @brief Bytes received so far; the packet size after COMPLETE. */
    [[nodiscard]] size_t GetLength() const { return length_; }
    /** @brief True when a packet in progress has timed out at @p now_ms. */
    [[nodiscard]] bool IsExpired(int64_t now_ms) const;

    /**
     * @brief Adds one frame.
     *
     * A start frame always begins a new packet, discarding one in progress. The last frame must be
     * exactly as long as SmpCanFramer makes it, including CAN FD padding.
     *
     * @param now_ms Current uptime, for the timeout.
     */
    Result Accept(std::span<const uint8_t> frame, int64_t now_ms);

    /** @brief True when the start flag of @p frame is set. */
    static bool IsStartFrame(std::span<const uint8_t> frame);
};

} // namespace eerie_leap::subsys::smp::can
