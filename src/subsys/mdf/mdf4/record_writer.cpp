#include <cstring>
#include <ios>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "record_writer.h"

namespace eerie_leap::subsys::mdf::mdf4 {

namespace {

template<typename T, size_t Index = 0>
constexpr uint8_t VariantIndexOf() {
    if constexpr(std::is_same_v<std::variant_alternative_t<Index, MdfValue>, T>)
        return static_cast<uint8_t>(Index);
    else
        return VariantIndexOf<T, Index + 1>();
}

uint8_t GetVariantIndex(const ChannelBlock& channel) {
    const auto bit_count = channel.GetBitCount();

    // A field that does not start on a byte boundary takes one byte more than its bits.
    if(channel.GetDataSizeBytes() * 8 != bit_count)
        throw std::runtime_error("Record channels must be whole bytes");

    switch(channel.GetDataType()) {
        case ChannelBlock::DataType::SignedIntegerLe:
            if(bit_count == 32)
                return VariantIndexOf<int32_t>();
            if(bit_count == 64)
                return VariantIndexOf<int64_t>();
            break;
        case ChannelBlock::DataType::UnsignedIntegerLe:
            if(bit_count == 32)
                return VariantIndexOf<uint32_t>();
            if(bit_count == 64)
                return VariantIndexOf<uint64_t>();
            break;
        case ChannelBlock::DataType::FloatLe:
            if(bit_count == 32)
                return VariantIndexOf<float>();
            if(bit_count == 64)
                return VariantIndexOf<double>();
            break;
        default:
            break;
    }

    throw std::runtime_error("Channel data type has no MdfValue type");
}

} // namespace

RecordWriter::RecordWriter(ChannelGroupBlock& channel_group) :
    channel_group_(&channel_group) {
    if((channel_group.GetFlags() & std::to_underlying(ChannelGroupBlock::Flag::VlsdChannel)) != 0)
        throw std::runtime_error("A VLSD channel group has no fixed length records");

    const auto invalidation_bytes = channel_group.GetInvalidationBytes();
    if(invalidation_bytes > kMaxInvalidationBytes) {
        throw std::runtime_error(
            "Record of " + std::to_string(invalidation_bytes) + " invalidation bytes exceeds the "
            + std::to_string(kMaxInvalidationBytes) + " byte limit"
        );
    }

    const auto record_id_size = channel_group.GetRecordIdSizeBytes();
    const uint64_t data_end = record_id_size + static_cast<uint64_t>(channel_group.GetDataSizeBytes());
    const uint64_t record_size = data_end + invalidation_bytes;

    if(record_size > kMaxRecordBytes) {
        throw std::runtime_error(
            "Record of " + std::to_string(record_size) + " bytes exceeds the " + std::to_string(kMaxRecordBytes)
            + " byte limit"
        );
    }

    const auto channels = channel_group.GetChannels();
    if(channels.size() > kMaxFields) {
        throw std::runtime_error(
            "Record of " + std::to_string(channels.size()) + " channels exceeds the " + std::to_string(kMaxFields)
            + " channel limit"
        );
    }

    for(const auto& channel : channels) {
        const uint64_t offset = record_id_size + static_cast<uint64_t>(channel->GetDataOffsetBytes());
        if(offset + channel->GetDataSizeBytes() > data_end)
            throw std::runtime_error("Channel does not fit into the record");

        if(channel->HasInvalidationBit()) {
            const auto position = channel->GetInvalidationBitPosition();
            if(position >= invalidation_bytes * 8) {
                throw std::runtime_error(
                    "Invalidation bit " + std::to_string(position) + " is outside the group's "
                    + std::to_string(invalidation_bytes) + " invalidation bytes"
                );
            }

            invalidation_bits_ |= 1U << position;
        }

        fields_[field_count_++] = Field{ .offset = static_cast<uint16_t>(offset),
            .size = static_cast<uint8_t>(channel->GetDataSizeBytes()),
            .variant_index = GetVariantIndex(*channel) };
    }

    const auto record_id = channel_group.GetRecordIdData();
    std::memcpy(record_.data(), record_id.data(), record_id_size);
    record_size_ = static_cast<uint16_t>(record_size);
    invalidation_offset_ = static_cast<uint16_t>(data_end);
    invalidation_bytes_ = static_cast<uint8_t>(invalidation_bytes);
}

uint64_t RecordWriter::Write(std::streambuf& stream, std::span<const MdfValue> values, uint32_t invalid_mask) {
    if(values.size() != field_count_)
        throw std::runtime_error("Invalid number of values");

    // A bit no channel refers to would invalidate nothing a reader shows.
    if((invalid_mask & ~invalidation_bits_) != 0)
        throw std::runtime_error("Invalidation mask sets a bit no channel uses");

    for(size_t i = 0; i < field_count_; i++) {
        const auto& field = fields_[i];

        // A 32 bit value in a 64 bit channel would leave half the field from the previous record.
        if(values[i].index() != field.variant_index)
            throw std::runtime_error("Value type does not match the channel data type");

        std::visit(
            [this, &field](const auto& value) {
                std::memcpy(record_.data() + field.offset, &value, sizeof(value));
            },
            values[i]
        );
    }

    // The low bytes of the mask, as the host is little endian: bit i is bit i % 8 of byte i / 8.
    if(invalidation_bytes_ > 0)
        std::memcpy(record_.data() + invalidation_offset_, &invalid_mask, invalidation_bytes_);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): streambuf::sputn takes char*, the buffer holds raw record bytes
    const auto written = stream.sputn(reinterpret_cast<const char*>(record_.data()), record_size_);
    if(std::cmp_not_equal(written, record_size_))
        throw std::ios_base::failure("Failed to write data record to stream.");

    channel_group_->IncrementCycleCount();
    bytes_written_ += record_size_;

    return record_size_;
}

} // namespace eerie_leap::subsys::mdf::mdf4
