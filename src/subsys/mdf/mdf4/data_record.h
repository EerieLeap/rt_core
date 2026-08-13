#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <streambuf>

#include "subsys/mdf/mdf_value.h"
#include "channel_group_block.h"

namespace eerie_leap::subsys::mdf::mdf4 {

using eerie_leap::subsys::mdf::MdfValue;

class DataRecord {
private:
    std::shared_ptr<ChannelGroupBlock> channel_group_;

    std::unique_ptr<uint8_t[]> Create(std::span<const MdfValue> values) const;
    static void WriteRecordData(std::streambuf& stream, const uint8_t* data, uint64_t size);

public:
    explicit DataRecord(std::shared_ptr<ChannelGroupBlock> channel_group);
    virtual ~DataRecord() = default;

    uint64_t GetRecordSizeBytes() const;

    /** Writes one record built from values matching the channel group channels, in order. */
    uint64_t WriteToStream(std::streambuf& stream, std::span<const MdfValue> values);
    /** Writes one record from an already packed data section of exactly cg_data_bytes length. */
    uint64_t WriteRawToStream(std::streambuf& stream, std::span<const uint8_t> data);
};

} // namespace eerie_leap::subsys::mdf::mdf4
