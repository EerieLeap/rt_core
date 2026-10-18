#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <streambuf>

#include "channel_group_block.h"
#include "subsys/mdf/mdf_value.h"

namespace eerie_leap::subsys::mdf::mdf4 {

using eerie_leap::subsys::mdf::MdfValue;

/**
 * Writes the fixed length records of one channel group. The layout is read from the group once, at
 * construction; Write() then fills a buffer the writer owns and hands the whole record to the
 * stream in one sputn(), without allocating. A record is the record ID, the data bytes and the
 * group's invalidation bytes, in that order.
 */
class RecordWriter {
public:
    static constexpr size_t MAX_FIELDS = 8;
    static constexpr size_t MAX_RECORD_BYTES = 64;
    // Write() takes the invalidation bits as one mask.
    static constexpr size_t MAX_INVALIDATION_BYTES = sizeof(uint32_t);

private:
    struct Field {
        uint16_t offset;
        uint8_t size;
        uint8_t variant_index;
    };

    std::array<uint8_t, MAX_RECORD_BYTES> record_{}; // the record ID bytes stay in front
    std::array<Field, MAX_FIELDS> fields_{};
    uint8_t field_count_ = 0;
    uint16_t record_size_ = 0;
    uint16_t invalidation_offset_ = 0;
    uint8_t invalidation_bytes_ = 0;
    uint32_t invalidation_bits_ = 0; // the bits the group's channels use
    uint64_t bytes_written_ = 0;
    ChannelGroupBlock* channel_group_ = nullptr;

public:
    /** Throws when the group's channels do not fit the limits above or have no MdfValue type. */
    explicit RecordWriter(ChannelGroupBlock& channel_group);

    // Move-only: the file sums its writers' byte counts into the DT length, so a copy's records
    // would be missing from it while the group's cycle count includes them.
    RecordWriter(const RecordWriter&) = delete;
    RecordWriter& operator=(const RecordWriter&) = delete;
    RecordWriter(RecordWriter&&) = default;
    RecordWriter& operator=(RecordWriter&&) = default;
    ~RecordWriter() = default;

    /**
     * Writes one record from one value per channel, in channel order, each holding the type of its
     * channel exactly. Bit i of @p invalid_mask marks the value of the channel at invalidation bit
     * position i invalid; a bit no channel of the group uses is rejected. A rejected record throws
     * std::runtime_error before anything is written; a short write throws std::ios_base::failure.
     */
    uint64_t Write(std::streambuf& stream, std::span<const MdfValue> values, uint32_t invalid_mask = 0);

    [[nodiscard]] uint16_t GetRecordSizeBytes() const {
        return record_size_;
    }
    [[nodiscard]] uint64_t GetBytesWritten() const {
        return bytes_written_;
    }
    [[nodiscard]] const ChannelGroupBlock* GetChannelGroup() const {
        return channel_group_;
    }
};

} // namespace eerie_leap::subsys::mdf::mdf4
