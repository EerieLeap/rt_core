#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "smp_can_frame_format.h"

namespace eerie_leap::subsys::smp::can {

/** @brief Splits a packet into SMP-CAN frames; only the last frame may be short. */
class SmpCanFramer {
private:
    std::span<const uint8_t> packet_;
    SmpCanFrameFormat format_;
    size_t offset_ = 0;
    uint8_t sequence_ = 0;

public:
    SmpCanFramer() = default;
    /** @param packet Must outlive the framer. */
    explicit SmpCanFramer(std::span<const uint8_t> packet, SmpCanFrameFormat format = {});

    /** @brief True once every frame has been passed by Advance(). */
    [[nodiscard]] bool IsDone() const { return offset_ >= packet_.size(); }

    /**
     * @brief Writes the current frame without advancing, so a failed send can be repeated.
     *
     * A short last CAN FD frame is zero-padded to the next valid CAN FD length.
     *
     * @return The frame length, 0 when done.
     */
    size_t Encode(std::span<uint8_t, SmpCanFrameFormat::MAX_FRAME_SIZE> frame) const;
    /** @brief Moves to the next frame. */
    void Advance();

    /** @brief Number of frames for a packet of @p packet_size bytes. */
    static constexpr size_t GetFrameCount(size_t packet_size, SmpCanFrameFormat format = {}) {
        return format.GetFrameCount(packet_size);
    }
};

} // namespace eerie_leap::subsys::smp::can
