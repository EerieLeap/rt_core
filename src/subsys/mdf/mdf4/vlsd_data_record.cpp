#include <cstdint>
#include <cstring>
#include <ios>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

#include "vlsd_data_record.h"

namespace eerie_leap::subsys::mdf::mdf4 {

VlsdDataRecord::VlsdDataRecord(std::shared_ptr<ChannelGroupBlock> vlsd_channel_group, uint64_t offset_channel_size_bytes)
    : vlsd_channel_group_(std::move(vlsd_channel_group)),
    offset_channel_size_bytes_(offset_channel_size_bytes),
    offset_(0) {

    if(!vlsd_channel_group_)
        throw std::invalid_argument("VLSD channel group cannot be null");

    if(!(vlsd_channel_group_->GetFlags() & std::to_underlying(ChannelGroupBlock::Flag::VlsdChannel)))
        throw std::runtime_error("Invalid channel group flags");

    if(offset_channel_size_bytes_ != 1 && offset_channel_size_bytes_ != 2
        && offset_channel_size_bytes_ != 4 && offset_channel_size_bytes_ != 8) {

        throw std::runtime_error("Invalid offset size bytes");
    }
}

void VlsdDataRecord::Reset() {
    offset_ = 0;
}

uint64_t VlsdDataRecord::GetOffset() const {
    return offset_;
}

uint64_t VlsdDataRecord::GetRecordSizeBytes(size_t data_size) const {
    return vlsd_channel_group_->GetRecordIdSizeBytes() + LENGTH_FIELD_SIZE_BYTES + data_size;
}

std::unique_ptr<uint8_t[]> VlsdDataRecord::Create(std::span<const uint8_t> data) const {
    auto size = GetRecordSizeBytes(data.size());
    auto buffer = std::make_unique<uint8_t[]>(size);

    auto id_data_bytes = vlsd_channel_group_->GetRecordIdData();
    std::memcpy(buffer.get(), id_data_bytes.data(), id_data_bytes.size());
    uint64_t offset = id_data_bytes.size();

    auto data_length = static_cast<uint32_t>(data.size());
    std::memcpy(buffer.get() + offset, &data_length, sizeof(data_length));
    offset += sizeof(data_length);

    std::memcpy(buffer.get() + offset, data.data(), data.size());

    return buffer;
}

std::vector<uint8_t> VlsdDataRecord::GetOffsetData() const {
    std::vector<uint8_t> buffer(offset_channel_size_bytes_);

    if(offset_channel_size_bytes_ == 1) {
        auto offset = static_cast<uint8_t>(offset_);
        std::memcpy(buffer.data(), &offset, offset_channel_size_bytes_);
    } else if(offset_channel_size_bytes_ == 2) {
        auto offset = static_cast<uint16_t>(offset_);
        std::memcpy(buffer.data(), &offset, offset_channel_size_bytes_);
    } else if(offset_channel_size_bytes_ == 4) {
        auto offset = static_cast<uint32_t>(offset_);
        std::memcpy(buffer.data(), &offset, offset_channel_size_bytes_);
    } else {
        std::memcpy(buffer.data(), &offset_, offset_channel_size_bytes_);
    }

    return buffer;
}

uint64_t VlsdDataRecord::WriteToStream(std::streambuf& stream, std::span<const uint8_t> data) {
    if(data.size() > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("VLSD data too large");

    const uint64_t next_offset = offset_ + LENGTH_FIELD_SIZE_BYTES + data.size();
    const uint64_t offset_limit = offset_channel_size_bytes_ >= 8
        ? std::numeric_limits<uint64_t>::max()
        : (1ULL << (offset_channel_size_bytes_ * 8));

    if(next_offset >= offset_limit)
        throw std::overflow_error("VLSD offset exceeds the offset channel range");

    const uint64_t record_size_bytes = GetRecordSizeBytes(data.size());
    const auto record_data = Create(data);

    const auto written = stream.sputn(
        reinterpret_cast<const char*>(record_data.get()),
        static_cast<std::streamsize>(record_size_bytes));

    if(written < 0 || static_cast<uint64_t>(written) != record_size_bytes)
        throw std::ios_base::failure("Failed to write VLSD data record to stream.");

    offset_ = next_offset;
    vlsd_channel_group_->IncrementCycleCount();
    vlsd_channel_group_->AddVlsdDataBytes(data.size());

    return record_size_bytes;
}

} // namespace eerie_leap::subsys::mdf::mdf4
