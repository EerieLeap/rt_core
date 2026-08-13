#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <streambuf>
#include <vector>

#include "channel_group_block.h"

namespace eerie_leap::subsys::mdf::mdf4 {

// Variable length signal data record
class VlsdDataRecord {
private:
    static constexpr uint64_t LENGTH_FIELD_SIZE_BYTES = 4;

    std::shared_ptr<ChannelGroupBlock> vlsd_channel_group_;
    uint64_t offset_channel_size_bytes_;
    uint64_t offset_;

    std::unique_ptr<uint8_t[]> Create(std::span<const uint8_t> data) const;
    uint64_t GetRecordSizeBytes(size_t data_size) const;

public:
    VlsdDataRecord(std::shared_ptr<ChannelGroupBlock> vlsd_channel_group, uint64_t offset_channel_size_bytes);
    virtual ~VlsdDataRecord() = default;

    void Reset();
    uint64_t GetOffset() const;
    std::vector<uint8_t> GetOffsetData() const;
    uint64_t WriteToStream(std::streambuf& stream, std::span<const uint8_t> data);
};

} // namespace eerie_leap::subsys::mdf::mdf4
