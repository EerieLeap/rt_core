#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace eerie_leap::subsys::smp::can {

/**
 * @brief SMP-CAN frame layout: one control byte `S:1 | seq:7`, then packet data.
 *
 * Classic CAN frames carry up to 7 packet bytes, CAN FD frames up to 63. The format follows the
 * type of the channel; every unit on a bus has to use the same one.
 */
class SmpCanFrameFormat {
private:
    static constexpr std::array<size_t, 7> CAN_FD_LENGTHS = {12, 16, 20, 24, 32, 48, 64};

    bool is_can_fd_ = false;

public:
    static constexpr size_t CLASSIC_FRAME_SIZE = 8;
    static constexpr size_t CAN_FD_FRAME_SIZE = 64;
    static constexpr size_t MAX_FRAME_SIZE = CAN_FD_FRAME_SIZE;
    static constexpr uint8_t START_FLAG = 0x80;    ///< Set in the first frame of a packet.
    static constexpr uint8_t SEQUENCE_MASK = 0x7F; ///< Frame index within the packet, wrapping at 128.

    /// Worst case for a full frame with a 29-bit ID, including bit stuffing.
    static constexpr uint32_t CLASSIC_FRAME_BIT_TIMES = 160;
    /// CAN FD arbitration, ACK, EOF and IFS, sent at the nominal bitrate.
    static constexpr uint32_t CAN_FD_NOMINAL_BIT_TIMES = 60;
    /// CAN FD control, 64 data bytes and CRC-21, sent at the data bitrate.
    static constexpr uint32_t CAN_FD_DATA_BIT_TIMES = 700;

    /** @brief Classic CAN frames. */
    constexpr SmpCanFrameFormat() = default;
    /** @param is_can_fd True for CAN FD frames. */
    constexpr explicit SmpCanFrameFormat(bool is_can_fd) : is_can_fd_(is_can_fd) {}

    [[nodiscard]] constexpr bool IsCanFd() const { return is_can_fd_; }

    /** @brief Length of every frame of a packet but the last. */
    [[nodiscard]] constexpr size_t GetFrameSize() const {
        return is_can_fd_ ? CAN_FD_FRAME_SIZE : CLASSIC_FRAME_SIZE;
    }

    /** @brief Packet bytes in every frame of a packet but the last. */
    [[nodiscard]] constexpr size_t GetDataSize() const { return GetFrameSize() - 1; }

    /** @brief Smallest frame length that holds @p size bytes; above 8, CAN FD only has fixed lengths. */
    [[nodiscard]] constexpr size_t GetPaddedSize(size_t size) const {
        if(!is_can_fd_ || size <= CLASSIC_FRAME_SIZE)
            return size;

        for(const size_t length : CAN_FD_LENGTHS) {
            if(size <= length)
                return length;
        }

        return size;
    }

    /** @brief Number of frames for a packet of @p packet_size bytes. */
    [[nodiscard]] constexpr size_t GetFrameCount(size_t packet_size) const {
        return (packet_size + GetDataSize() - 1) / GetDataSize();
    }

    /**
     * @brief Worst-case duration of a full frame, 0 for a 0 bitrate.
     * @param data_bitrate CAN FD data phase bitrate; 0 uses the nominal bitrate.
     */
    [[nodiscard]] constexpr uint32_t GetFrameTimeNs(uint32_t bitrate, uint32_t data_bitrate = 0) const {
        constexpr uint64_t NANOS_PER_SECOND = 1'000'000'000;

        if(bitrate == 0)
            return 0;

        if(!is_can_fd_)
            return static_cast<uint32_t>(CLASSIC_FRAME_BIT_TIMES * NANOS_PER_SECOND / bitrate);

        if(data_bitrate == 0)
            data_bitrate = bitrate;

        return static_cast<uint32_t>(
            CAN_FD_NOMINAL_BIT_TIMES * NANOS_PER_SECOND / bitrate
            + CAN_FD_DATA_BIT_TIMES * NANOS_PER_SECOND / data_bitrate);
    }

    constexpr bool operator==(const SmpCanFrameFormat&) const = default;
};

} // namespace eerie_leap::subsys::smp::can
