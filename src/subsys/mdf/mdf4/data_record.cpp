#include <cstdint>
#include <cstring>
#include <ios>
#include <memory>
#include <stdexcept>

#include "subsys/mdf/mdf_helpers.h"

#include "data_record.h"

namespace eerie_leap::subsys::mdf::mdf4 {

DataRecord::DataRecord(std::shared_ptr<ChannelGroupBlock> channel_group)
    : channel_group_(std::move(channel_group)) {

    if(!channel_group_)
        throw std::invalid_argument("Channel group cannot be null");
}

uint64_t DataRecord::GetRecordSizeBytes() const {
    return channel_group_->GetRecordIdSizeBytes() + channel_group_->GetDataSizeBytes();
}

std::unique_ptr<uint8_t[]> DataRecord::Create(std::span<const MdfValue> values) const {
    auto size = GetRecordSizeBytes();
    auto buffer = std::make_unique<uint8_t[]>(size);

    auto id_data_bytes = channel_group_->GetRecordIdData();
    std::memcpy(buffer.get(), id_data_bytes.data(), id_data_bytes.size());
    const uint64_t id_offset = id_data_bytes.size();

    auto channels = channel_group_->GetChannels();
    if(channels.size() != values.size())
        throw std::runtime_error("Invalid number of values");

    for(size_t i = 0; i < channels.size(); i++) {
        const auto& channel = channels[i];
        const auto expected = MdfHelpers::ToMdf4ChannelDataType(GetMdfDataType(values[i]));

        if(expected.data_type != channel->GetDataType() || expected.bit_count != channel->GetBitCount())
            throw std::runtime_error("Value type does not match the channel data type");

        const uint64_t offset = id_offset + channel->GetDataOffsetBytes();
        if(offset + channel->GetDataSizeBytes() > size)
            throw std::runtime_error("Channel does not fit into the record");

        std::visit([&](auto&& value) {
            std::memcpy(buffer.get() + offset, &value, sizeof(value));
        }, values[i]);
    }

    return buffer;
}

uint64_t DataRecord::WriteToStream(std::streambuf& stream, std::span<const MdfValue> values) {
    const auto size = GetRecordSizeBytes();
    const auto record_data = Create(values);

    WriteRecordData(stream, record_data.get(), size);
    channel_group_->IncrementCycleCount();

    return size;
}

uint64_t DataRecord::WriteRawToStream(std::streambuf& stream, std::span<const uint8_t> data) {
    if(data.size() != channel_group_->GetDataSizeBytes())
        throw std::runtime_error("Record data size does not match the channel group record size");

    auto id_data_bytes = channel_group_->GetRecordIdData();

    WriteRecordData(stream, id_data_bytes.data(), id_data_bytes.size());
    WriteRecordData(stream, data.data(), data.size());

    channel_group_->IncrementCycleCount();

    return id_data_bytes.size() + data.size();
}

void DataRecord::WriteRecordData(std::streambuf& stream, const uint8_t* data, uint64_t size) {
    const auto written = stream.sputn(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));

    if(written < 0 || static_cast<uint64_t>(written) != size)
        throw std::ios_base::failure("Failed to write data record to stream.");
}

} // namespace eerie_leap::subsys::mdf::mdf4
