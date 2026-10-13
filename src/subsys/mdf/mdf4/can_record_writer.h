#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <streambuf>

#include "subsys/canbus/can_frame.h"
#include "subsys/mdf/mdf_data_type.h"
#include "channel_group_block.h"

namespace eerie_leap::subsys::mdf::mdf4 {

using eerie_leap::subsys::canbus::CanFrame;
using eerie_leap::subsys::canbus::CAN_FRAME_MAX_DATA_LENGTH;

/**
 * Writes the records of one CAN_DataFrame channel group and its VLSD group: per frame the VLSD
 * record with the payload, then the frame record pointing at it, assembled in a buffer the writer
 * owns and handed to the stream in one sputn(), without allocating.
 */
class CanRecordWriter {
public:
    // CAN_DataFrame record layout, byte offsets relative to the start of the composite channel,
    // which follows the master time channel. Laid out like mdflib so every reader finds the members.
    static constexpr uint32_t BUS_CHANNEL_OFFSET = 0;      // BusChannel(8)
    static constexpr uint32_t ID_OFFSET = 1;               // ID(29) | reserved(2) | IDE(1)
    static constexpr uint32_t FLAGS_OFFSET = 5;            // DLC(4) | Dir(1) | EDL(1) | BRS(1) | ESI(1)
    static constexpr uint32_t DATA_LENGTH_OFFSET = 6;      // DataLength(8)
    static constexpr uint32_t DATA_BYTES_OFFSET = 7;       // VLSD offset (vlsd_offset_size_bytes)

    static constexpr uint32_t IDE_BIT = 31;
    static constexpr uint32_t DLC_BIT = 0;
    static constexpr uint32_t DIR_BIT = 4;
    static constexpr uint32_t EDL_BIT = 5;
    static constexpr uint32_t BRS_BIT = 6;
    static constexpr uint32_t ESI_BIT = 7;

    static constexpr uint32_t FrameSizeBytes(uint8_t vlsd_offset_size_bytes) {
        return DATA_BYTES_OFFSET + vlsd_offset_size_bytes;
    }

private:
    static constexpr size_t MAX_RECORD_ID_BYTES = 8;
    static constexpr size_t MAX_TIME_BYTES = 8;
    static constexpr size_t MAX_VLSD_OFFSET_BYTES = 8;
    static constexpr size_t VLSD_LENGTH_BYTES = 4;
    static constexpr size_t MAX_WRITE_BYTES =
        MAX_RECORD_ID_BYTES + VLSD_LENGTH_BYTES + CAN_FRAME_MAX_DATA_LENGTH
        + MAX_RECORD_ID_BYTES + MAX_TIME_BYTES + DATA_BYTES_OFFSET + MAX_VLSD_OFFSET_BYTES;

    std::array<uint8_t, MAX_WRITE_BYTES> buffer_{};
    std::array<uint8_t, MAX_RECORD_ID_BYTES> record_id_{};
    std::array<uint8_t, MAX_RECORD_ID_BYTES> vlsd_record_id_{};
    uint8_t record_id_size_ = 0;
    uint8_t vlsd_record_id_size_ = 0;
    uint8_t time_size_ = 0;
    uint8_t vlsd_offset_size_ = 0;
    uint16_t record_size_ = 0;
    uint64_t vlsd_offset_ = 0;
    uint64_t bytes_written_ = 0;
    ChannelGroupBlock* channel_group_ = nullptr;
    ChannelGroupBlock* vlsd_channel_group_ = nullptr;

public:
    /** Throws when the groups do not have the CAN_DataFrame layout for @p time_type and the offset width. */
    CanRecordWriter(
        ChannelGroupBlock& channel_group,
        ChannelGroupBlock& vlsd_channel_group,
        MdfDataType time_type,
        uint8_t vlsd_offset_size_bytes);

    // Move-only, like RecordWriter: a copy would also hand out VLSD offsets of its own.
    CanRecordWriter(const CanRecordWriter&) = delete;
    CanRecordWriter& operator=(const CanRecordWriter&) = delete;
    CanRecordWriter(CanRecordWriter&&) = default;
    CanRecordWriter& operator=(CanRecordWriter&&) = default;

    /**
     * Writes the payload record and the frame record. Throws std::overflow_error, before anything is
     * written, when the next VLSD offset would not fit the offset channel, and std::ios_base::failure
     * on a short write.
     */
    uint64_t Write(std::streambuf& stream, const CanFrame& frame, double time, uint8_t bus_channel);

    /** The VLSD offset the next payload is written at: 0 in a new file. */
    void Reset(uint64_t vlsd_offset = 0);

    [[nodiscard]] uint64_t GetVlsdOffset() const { return vlsd_offset_; }
    [[nodiscard]] uint16_t GetRecordSizeBytes() const { return record_size_; }
    [[nodiscard]] uint64_t GetBytesWritten() const { return bytes_written_; }
    [[nodiscard]] const ChannelGroupBlock* GetChannelGroup() const { return channel_group_; }
};

} // namespace eerie_leap::subsys::mdf::mdf4
