#include <cstring>
#include <ios>
#include <stdexcept>
#include <utility>

#include "can_record_writer.h"

namespace eerie_leap::subsys::mdf::mdf4 {

namespace {

constexpr uint32_t CAN_ID_STANDARD_MASK = 0x7FF;
constexpr uint32_t CAN_ID_EXTENDED_MASK = 0x1FFFFFFF;

// ISO 11898-1 data length code table; codes above 8 only exist for CAN FD.
uint8_t ToCanDlc(size_t data_length) {
    if(data_length <= 8)
        return static_cast<uint8_t>(data_length);

    constexpr std::array<size_t, 7> FD_LENGTHS{ 12, 16, 20, 24, 32, 48, 64 };
    for(size_t i = 0; i < FD_LENGTHS.size(); i++) {
        if(data_length <= FD_LENGTHS[i])
            return static_cast<uint8_t>(9 + i);
    }

    throw std::runtime_error("CAN frame payload too large");
}

} // namespace

CanRecordWriter::CanRecordWriter(
    ChannelGroupBlock& channel_group,
    ChannelGroupBlock& vlsd_channel_group,
    MdfDataType time_type,
    uint8_t vlsd_offset_size_bytes
) :
    channel_group_(&channel_group),
    vlsd_channel_group_(&vlsd_channel_group) {

    if((vlsd_channel_group.GetFlags() & std::to_underlying(ChannelGroupBlock::Flag::VlsdChannel)) == 0)
        throw std::runtime_error("Invalid channel group flags");

    if(time_type == MdfDataType::Float32)
        time_size_ = sizeof(float);
    else if(time_type == MdfDataType::Float64)
        time_size_ = sizeof(double);
    else
        throw std::invalid_argument("Master time channel must be Float32 or Float64");

    if(vlsd_offset_size_bytes != 4 && vlsd_offset_size_bytes != 8)
        throw std::invalid_argument("VLSD offset size must be 4 or 8 bytes");

    vlsd_offset_size_ = vlsd_offset_size_bytes;

    if(channel_group.GetDataSizeBytes() != time_size_ + FrameSizeBytes(vlsd_offset_size_))
        throw std::runtime_error("Unexpected CAN data frame record size");

    if(channel_group.GetInvalidationBytes() != 0)
        throw std::runtime_error("A CAN data frame record has no invalidation bytes");

    // The total alone cannot tell Float64 time with 4-byte offsets from Float32 with 8-byte ones.
    const auto channels = channel_group.GetChannels();
    if(channels.size() < 2
        || channels[0]->GetDataType() != ChannelBlock::DataType::FloatLe
        || channels[0]->GetBitCount() != time_size_ * 8
        || channels[1]->GetBitCount() != FrameSizeBytes(vlsd_offset_size_) * 8) {

        throw std::runtime_error("CAN data frame group does not match the time type and offset width");
    }

    record_id_size_ = channel_group.GetRecordIdSizeBytes();
    record_id_ = channel_group.GetRecordIdData();
    vlsd_record_id_size_ = vlsd_channel_group.GetRecordIdSizeBytes();
    vlsd_record_id_ = vlsd_channel_group.GetRecordIdData();

    record_size_ = static_cast<uint16_t>(record_id_size_ + channel_group.GetDataSizeBytes());
}

void CanRecordWriter::Reset(uint64_t vlsd_offset) {
    vlsd_offset_ = vlsd_offset;
}

uint64_t CanRecordWriter::Write(std::streambuf& stream, const CanFrame& frame, double time, uint8_t bus_channel) {
    const size_t data_length = frame.data.size();
    if(data_length > CAN_FRAME_MAX_DATA_LENGTH)
        throw std::runtime_error("CAN frame payload too large");

    const uint64_t next_vlsd_offset = vlsd_offset_ + VLSD_LENGTH_BYTES + data_length;
    if(vlsd_offset_size_ < sizeof(uint64_t) && next_vlsd_offset >= (1ULL << (vlsd_offset_size_ * 8U)))
        throw std::overflow_error("VLSD offset exceeds the offset channel range");

    // Payload first: a file cut between the two records then ends in an orphan payload, which
    // readers ignore, instead of a frame pointing past the end, which breaks the whole CAN group.
    uint8_t* vlsd_record = buffer_.data();
    std::memcpy(vlsd_record, vlsd_record_id_.data(), vlsd_record_id_size_);

    const auto vlsd_length = static_cast<uint32_t>(data_length);
    std::memcpy(vlsd_record + vlsd_record_id_size_, &vlsd_length, sizeof(vlsd_length));
    std::memcpy(vlsd_record + vlsd_record_id_size_ + VLSD_LENGTH_BYTES, frame.data.data(), data_length);

    const size_t vlsd_record_size = vlsd_record_id_size_ + VLSD_LENGTH_BYTES + data_length;

    uint8_t* record = vlsd_record + vlsd_record_size;
    std::memcpy(record, record_id_.data(), record_id_size_);

    // Timestamp
    uint8_t* time_field = record + record_id_size_;
    if(time_size_ == sizeof(float)) {
        const auto time_float = static_cast<float>(time);
        std::memcpy(time_field, &time_float, sizeof(time_float));
    } else {
        std::memcpy(time_field, &time, sizeof(time));
    }

    uint8_t* can_frame = time_field + time_size_;

    // CAN_DataFrame.BusChannel (8 bits)
    can_frame[BUS_CHANNEL_OFFSET] = bus_channel;

    // CAN_DataFrame.ID (29 bits) | CAN_DataFrame.IDE (bit 31)
    const uint32_t frame_id = frame.id & (frame.is_extended ? CAN_ID_EXTENDED_MASK : CAN_ID_STANDARD_MASK);
    const uint32_t frame_ide = frame.is_extended ? 1U : 0U;

    const uint32_t id_pack = frame_id | (frame_ide << IDE_BIT);
    std::memcpy(can_frame + ID_OFFSET, &id_pack, sizeof(id_pack));

    // CAN_DataFrame.DLC (4 bits) | Dir (1 bit) | EDL (1 bit) | BRS (1 bit) | ESI (1 bit)
    const uint32_t frame_dlc = ToCanDlc(data_length);
    const uint32_t frame_dir = frame.is_transmit ? 1U : 0U;
    const uint32_t frame_edl = frame.is_can_fd ? 1U : 0U;
    const uint32_t frame_brs = frame.is_bitrate_switch ? 1U : 0U;
    const uint32_t frame_esi = 0U;

    can_frame[FLAGS_OFFSET] = static_cast<uint8_t>(
        ((frame_dlc & 0xFU) << DLC_BIT) | ((frame_dir & 0x1U) << DIR_BIT) | ((frame_edl & 0x1U) << EDL_BIT)
        | ((frame_brs & 0x1U) << BRS_BIT) | ((frame_esi & 0x1U) << ESI_BIT)
    );

    // CAN_DataFrame.DataLength (8 bits)
    can_frame[DATA_LENGTH_OFFSET] = static_cast<uint8_t>(data_length);

    // CAN_DataFrame.DataBytes, offset of the VLSD record in front of this one
    std::memcpy(can_frame + DATA_BYTES_OFFSET, &vlsd_offset_, vlsd_offset_size_);

    const auto size = static_cast<std::streamsize>(vlsd_record_size + record_size_);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): streambuf::sputn takes char*, the buffer holds raw record bytes
    if(stream.sputn(reinterpret_cast<const char*>(buffer_.data()), size) != size)
        throw std::ios_base::failure("Failed to write CAN data frame records to stream.");

    vlsd_channel_group_->IncrementCycleCount();
    vlsd_channel_group_->AddVlsdDataBytes(data_length);
    channel_group_->IncrementCycleCount();

    vlsd_offset_ = next_vlsd_offset;
    bytes_written_ += static_cast<uint64_t>(size);

    return static_cast<uint64_t>(size);
}

} // namespace eerie_leap::subsys::mdf::mdf4
