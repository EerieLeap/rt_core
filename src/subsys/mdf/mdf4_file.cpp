#include <array>
#include <cstring>
#include <ios>
#include <stdexcept>
#include <utility>
#include <vector>

#include "subsys/time/time_helpers.hpp"
#include "subsys/mdf/mdf4/channel_block.h"
#include "subsys/mdf/mdf4/channel_group_block.h"
#include "subsys/mdf/mdf4/data_group_block.h"
#include "subsys/mdf/mdf4/source_information_block.h"
#include "mdf_helpers.h"

#include "mdf4_file.h"

namespace eerie_leap::subsys::mdf {

using namespace eerie_leap::subsys::time;

namespace {

// CAN_DataFrame record layout, byte offsets relative to the start of the record data section.
constexpr uint32_t CAN_TIMESTAMP_OFFSET = 0;
constexpr uint32_t CAN_ID_OFFSET = 4;               // BusChannel(2) | ID(29) | IDE(1)
constexpr uint32_t CAN_DIR_LENGTH_OFFSET = 8;       // Dir(1) | DataLength(7)
constexpr uint32_t CAN_EDL_BRS_DLC_OFFSET = 9;      // EDL(1) | BRS(1) | DLC(4)
constexpr uint32_t CAN_DATA_BYTES_OFFSET = 10;      // VLSD offset (4 bytes)

constexpr uint32_t CAN_FRAME_BIT_COUNT = 80;
constexpr uint32_t CAN_DATA_BYTES_BIT_COUNT = 32;
constexpr uint32_t CAN_RECORD_DATA_SIZE = 14;

constexpr uint32_t CAN_ID_STANDARD_MASK = 0x7FF;
constexpr uint32_t CAN_ID_EXTENDED_MASK = 0x1FFFFFFF;
constexpr size_t CAN_MAX_DATA_LENGTH = 64;

// ISO 11898-1 data length code table; codes above 8 only exist for CAN FD.
uint8_t ToCanDlc(size_t data_length) {
    if(data_length <= 8)
        return static_cast<uint8_t>(data_length);

    constexpr std::array<size_t, 7> fd_lengths{12, 16, 20, 24, 32, 48, 64};
    for(size_t i = 0; i < fd_lengths.size(); i++) {
        if(data_length <= fd_lengths[i])
            return static_cast<uint8_t>(9 + i);
    }

    throw std::runtime_error("CAN frame payload too large");
}

} // namespace

Mdf4File::Mdf4File(uint8_t record_id_size_bytes)
    : data_bytes_written_(0), is_header_written_(false), is_finalized_(false) {

    if(record_id_size_bytes == 0)
        throw std::invalid_argument("Record ID size must be at least 1 byte");

    id_block_ = std::make_unique<mdf4::IdBlock>();
    header_block_ = std::make_unique<mdf4::HeaderBlock>();

    data_group_ = std::make_shared<mdf4::DataGroupBlock>(record_id_size_bytes);
    header_block_->AddDataGroup(data_group_);
}

// NOTE: Incorrect start time in Header Block time seems to
// cause asammdf gui to fail parsing the file
void Mdf4File::UpdateCurrentTime(std::chrono::system_clock::time_point time) {
    header_block_->SetCurrentTimeNs(TimeHelpers::ToUint64(time));
}

std::shared_ptr<mdf4::DataGroupBlock> Mdf4File::GetDataGroup() const {
    return data_group_;
}

void Mdf4File::EnsureRecordsWritable() const {
    if(!is_header_written_)
        throw std::runtime_error("File header has not been written yet");

    if(is_finalized_)
        throw std::runtime_error("File has already been finalized");
}

std::shared_ptr<mdf4::ChannelGroupBlock> Mdf4File::CreateChannelGroupBlock(uint64_t record_id) {
    if(is_header_written_)
        throw std::runtime_error("Channel groups cannot be added after the header was written");

    if(record_ids_.contains(record_id))
        throw std::runtime_error("Record ID already exists");

    // Throws when the record ID does not fit into the data group record ID size.
    auto channel_group = std::make_shared<mdf4::ChannelGroupBlock>(
        data_group_->GetRecordIdSizeBytes(),
        record_id);

    record_ids_.insert(record_id);
    channel_groups_.push_back(channel_group);
    data_group_->AddChannelGroup(channel_group);

    return channel_group;
}

std::shared_ptr<mdf4::ChannelGroupBlock> Mdf4File::CreateChannelGroup(uint64_t record_id, const std::string& name) {
    auto channel_group = CreateChannelGroupBlock(record_id);
    channel_group->SetName(GetOrCreateTextBlock(name));

    auto channel_data_type = MdfHelpers::ToMdf4ChannelDataType(MdfDataType::Float32);
    auto channel_time = std::make_shared<mdf4::ChannelBlock>(
        mdf4::ChannelBlock::Type::Master,
        mdf4::ChannelBlock::SyncType::Time,
        channel_data_type.data_type,
        channel_data_type.bit_count);
    channel_time->SetName(GetOrCreateTextBlock("Timestamp"));
    channel_time->SetUnit(GetOrCreateTextBlock("s"));
    channel_group->AddChannel(channel_time);

    data_records_.emplace(channel_group, std::make_shared<mdf4::DataRecord>(channel_group));

    return channel_group;
}

std::shared_ptr<mdf4::ChannelGroupBlock> Mdf4File::CreateVLSDChannelGroup(uint64_t record_id) {
    auto channel_group = CreateChannelGroupBlock(record_id);
    channel_group->SetFlags(std::to_underlying(mdf4::ChannelGroupBlock::Flag::VlsdChannel));

    return channel_group;
}

std::shared_ptr<mdf4::ChannelGroupBlock> Mdf4File::CreateCanDataFrameChannelGroup(
    const std::shared_ptr<mdf4::ChannelGroupBlock>& vlsd_channel_group,
    uint64_t record_id,
    const std::string& name) {

    if(!vlsd_channel_group)
        throw std::invalid_argument("VLSD channel group cannot be null");

    if(!(vlsd_channel_group->GetFlags() & std::to_underlying(mdf4::ChannelGroupBlock::Flag::VlsdChannel)))
        throw std::runtime_error("Invalid channel group flags");

    auto channel_group = CreateChannelGroup(record_id, name);
    channel_group->SetFlags(
        std::to_underlying(mdf4::ChannelGroupBlock::Flag::BusEvent)
        | std::to_underlying(mdf4::ChannelGroupBlock::Flag::PlainBusEvent));
    channel_group->SetPathSeparator('.');

    auto source_information = std::make_shared<mdf4::SourceInformationBlock>(
        mdf4::SourceInformationBlock::SourceType::Bus,
        mdf4::SourceInformationBlock::BusType::Can);
    source_information->SetName(GetOrCreateTextBlock("CAN"));
    source_information->SetPath(GetOrCreateTextBlock("CAN"));
    channel_group->AddSourceInformation(source_information);

    auto can_data_frame_channel = CreateChannelBlock(MdfDataType::ByteArray, "CAN_DataFrame");
    can_data_frame_channel->SetFlags(std::to_underlying(mdf4::ChannelBlock::Flag::BusEvent));
    can_data_frame_channel->SetBitCount(CAN_FRAME_BIT_COUNT);
    channel_group->AddChannel(can_data_frame_channel);

    auto can_data_frame_bus_channel = CreateChannelBlock(MdfDataType::Uint32, "CAN_DataFrame.BusChannel");
    can_data_frame_bus_channel->SetFlags(std::to_underlying(mdf4::ChannelBlock::Flag::BusEvent));
    can_data_frame_bus_channel->SetOffsetBytes(CAN_ID_OFFSET);
    can_data_frame_bus_channel->SetBitCount(2);
    can_data_frame_channel->SetArrayBlock(can_data_frame_bus_channel);

    auto can_data_frame_id_channel = CreateChannelBlock(MdfDataType::Uint32, "CAN_DataFrame.ID");
    can_data_frame_id_channel->SetFlags(std::to_underlying(mdf4::ChannelBlock::Flag::BusEvent));
    can_data_frame_id_channel->SetOffsetBytes(CAN_ID_OFFSET);
    can_data_frame_id_channel->SetOffsetBits(2);
    can_data_frame_id_channel->SetBitCount(29);
    can_data_frame_bus_channel->LinkBlock(can_data_frame_id_channel);

    // IDE (Identifier Extension) | 0 - 11 bit ID, 1 - 29 bit ID
    auto can_data_frame_ide_channel = CreateChannelBlock(MdfDataType::Uint32, "CAN_DataFrame.IDE");
    can_data_frame_ide_channel->SetFlags(std::to_underlying(mdf4::ChannelBlock::Flag::BusEvent));
    can_data_frame_ide_channel->SetOffsetBytes(CAN_ID_OFFSET + 3);
    can_data_frame_ide_channel->SetOffsetBits(7);
    can_data_frame_ide_channel->SetBitCount(1);
    can_data_frame_bus_channel->LinkBlock(can_data_frame_ide_channel);

    // Dir (Direction) | 0 - Receive, 1 - Transmit
    auto can_data_frame_dir_channel = CreateChannelBlock(MdfDataType::Uint32, "CAN_DataFrame.Dir");
    can_data_frame_dir_channel->SetFlags(std::to_underlying(mdf4::ChannelBlock::Flag::BusEvent));
    can_data_frame_dir_channel->SetOffsetBytes(CAN_DIR_LENGTH_OFFSET);
    can_data_frame_dir_channel->SetBitCount(1);
    can_data_frame_bus_channel->LinkBlock(can_data_frame_dir_channel);

    auto can_data_frame_data_length_channel = CreateChannelBlock(MdfDataType::Uint32, "CAN_DataFrame.DataLength");
    can_data_frame_data_length_channel->SetFlags(std::to_underlying(mdf4::ChannelBlock::Flag::BusEvent));
    can_data_frame_data_length_channel->SetOffsetBytes(CAN_DIR_LENGTH_OFFSET);
    can_data_frame_data_length_channel->SetOffsetBits(1);
    can_data_frame_data_length_channel->SetBitCount(7);
    can_data_frame_bus_channel->LinkBlock(can_data_frame_data_length_channel);

    // EDL (Extended Data Length) | 0 - Standard CAN, 1 - CAN FD
    auto can_data_frame_edl_channel = CreateChannelBlock(MdfDataType::Uint32, "CAN_DataFrame.EDL");
    can_data_frame_edl_channel->SetFlags(std::to_underlying(mdf4::ChannelBlock::Flag::BusEvent));
    can_data_frame_edl_channel->SetOffsetBytes(CAN_EDL_BRS_DLC_OFFSET);
    can_data_frame_edl_channel->SetBitCount(1);
    can_data_frame_bus_channel->LinkBlock(can_data_frame_edl_channel);

    // BRS (Bit Rate Switch)
    auto can_data_frame_brs_channel = CreateChannelBlock(MdfDataType::Uint32, "CAN_DataFrame.BRS");
    can_data_frame_brs_channel->SetFlags(std::to_underlying(mdf4::ChannelBlock::Flag::BusEvent));
    can_data_frame_brs_channel->SetOffsetBytes(CAN_EDL_BRS_DLC_OFFSET);
    can_data_frame_brs_channel->SetOffsetBits(1);
    can_data_frame_brs_channel->SetBitCount(1);
    can_data_frame_bus_channel->LinkBlock(can_data_frame_brs_channel);

    // DLC (Data Length Code)
    auto can_data_frame_dlc_channel = CreateChannelBlock(MdfDataType::Uint32, "CAN_DataFrame.DLC");
    can_data_frame_dlc_channel->SetFlags(std::to_underlying(mdf4::ChannelBlock::Flag::BusEvent));
    can_data_frame_dlc_channel->SetOffsetBytes(CAN_EDL_BRS_DLC_OFFSET);
    can_data_frame_dlc_channel->SetOffsetBits(2);
    can_data_frame_dlc_channel->SetBitCount(4);
    can_data_frame_bus_channel->LinkBlock(can_data_frame_dlc_channel);

    // VLSD data block offset
    auto can_data_frame_data_bytes_channel = CreateChannelBlock(MdfDataType::ByteArray, "CAN_DataFrame.DataBytes");
    can_data_frame_data_bytes_channel->SetType(mdf4::ChannelBlock::Type::VariableLength);
    can_data_frame_data_bytes_channel->SetFlags(std::to_underlying(mdf4::ChannelBlock::Flag::BusEvent));
    can_data_frame_data_bytes_channel->SetOffsetBytes(CAN_DATA_BYTES_OFFSET);
    can_data_frame_data_bytes_channel->SetBitCount(CAN_DATA_BYTES_BIT_COUNT);
    can_data_frame_data_bytes_channel->SetSignalDataBlock(vlsd_channel_group);
    can_data_frame_bus_channel->LinkBlock(can_data_frame_data_bytes_channel);

    if(channel_group->GetDataSizeBytes() != CAN_RECORD_DATA_SIZE)
        throw std::runtime_error("Unexpected CAN data frame record size");

    // The composite CAN channel replaces the plain per channel record layout.
    data_records_.erase(channel_group);

    CanDataFrameBlocks can_data_frame_blocks = {
        .header_data_record = std::make_shared<mdf4::DataRecord>(channel_group),
        .raw_data_vlsd_data_record = std::make_shared<mdf4::VlsdDataRecord>(
            vlsd_channel_group,
            can_data_frame_data_bytes_channel->GetDataSizeBytes()
        )
    };
    can_data_frame_blocks_.emplace(channel_group, can_data_frame_blocks);

    return channel_group;
}

std::shared_ptr<mdf4::ChannelBlock> Mdf4File::CreateChannelBlock(MdfDataType data_type, const std::string& name, const std::string& unit) {
    auto channel_data_type = MdfHelpers::ToMdf4ChannelDataType(data_type);
    auto channel = std::make_shared<mdf4::ChannelBlock>(
        mdf4::ChannelBlock::Type::FixedLength,
        mdf4::ChannelBlock::SyncType::NoSync,
        channel_data_type.data_type,
        channel_data_type.bit_count);

    if(!name.empty())
        channel->SetName(GetOrCreateTextBlock(name));
    if(!unit.empty())
        channel->SetUnit(GetOrCreateTextBlock(unit));

    return channel;
}

std::shared_ptr<mdf4::ChannelBlock> Mdf4File::CreateDataChannel(
    const std::shared_ptr<mdf4::ChannelGroupBlock>& channel_group, MdfDataType data_type, const std::string& name, const std::string& unit) {

    if(is_header_written_)
        throw std::runtime_error("Channels cannot be added after the header was written");

    if(!data_records_.contains(channel_group))
        throw std::runtime_error("Channels can only be added to a plain channel group");

    auto channel = CreateChannelBlock(data_type, name, unit);
    channel_group->AddChannel(channel);

    return channel;
}

std::shared_ptr<mdf4::ChannelConversionBlock> Mdf4File::CreateAlgebraicConversion(const std::string& formula) {
    return mdf4::ChannelConversionBlock::CreateAlgebraicConversion(GetOrCreateTextBlock(formula));
}

std::shared_ptr<mdf4::TextBlock> Mdf4File::GetOrCreateTextBlock(const std::string& name) {
    auto text_block = text_blocks_.find(name);
    if(text_block != text_blocks_.end())
        return text_block->second;

    auto new_text_block = std::make_shared<mdf4::TextBlock>();
    new_text_block->SetText(name);
    text_blocks_.emplace(name, new_text_block);

    return new_text_block;
}

uint64_t Mdf4File::WriteHeaderToStream(std::streambuf& stream) {
    auto data_block = data_group_->GetDataBlock();
    if(!data_block)
        throw std::runtime_error("Data group has no data block");

    for(auto& channel_group : channel_groups_)
        channel_group->ResetCounters();

    for(auto& [_, can_data_frame_block] : can_data_frame_blocks_)
        can_data_frame_block.raw_data_vlsd_data_record->Reset();

    data_block->SetDataSizeBytes(0);
    data_bytes_written_ = 0;
    is_header_written_ = false;
    is_finalized_ = false;

    id_block_->SetFinalized(false);
    id_block_->AddStandardFlag(mdf4::IdBlock::StandardFlag::InvalidCGCount);
    id_block_->AddStandardFlag(mdf4::IdBlock::StandardFlag::InvalidLastDTBlock);
    id_block_->AddStandardFlag(mdf4::IdBlock::StandardFlag::InvalidDataVLSDBlock);

    id_block_->Reset();
    header_block_->Reset();

    auto current_address = id_block_->ResolveAddress(0);
    header_block_->ResolveAddress(current_address);

    auto bytes_written = id_block_->WriteToStream(stream);
    bytes_written += header_block_->WriteToStream(stream);

    // Records are appended straight after the DT header, so it has to be the last block written.
    if(bytes_written != data_block->GetAddress() + data_block->GetSerializedSize())
        throw std::runtime_error("DT block is not the last block of the file header");

    is_header_written_ = true;

    return bytes_written;
}

uint64_t Mdf4File::WriteDataRecordToStream(
    const std::shared_ptr<mdf4::ChannelGroupBlock>& channel_group,
    std::streambuf& stream,
    std::span<const MdfValue> values) {

    EnsureRecordsWritable();

    auto data_record = data_records_.find(channel_group);
    if(data_record == data_records_.end())
        throw std::runtime_error("Invalid channel group");

    auto bytes_written = data_record->second->WriteToStream(stream, values);
    data_bytes_written_ += bytes_written;

    return bytes_written;
}

uint64_t Mdf4File::WriteCanbusDataRecordToStream(
        const std::shared_ptr<mdf4::ChannelGroupBlock>& channel_group,
        std::streambuf& stream,
        const CanFrame& can_frame,
        float time) {

    EnsureRecordsWritable();

    auto can_data_frame_block = can_data_frame_blocks_.find(channel_group);
    if(can_data_frame_block == can_data_frame_blocks_.end())
        throw std::runtime_error("Invalid channel group");

    if(can_frame.data.size() > CAN_MAX_DATA_LENGTH)
        throw std::runtime_error("CAN frame payload too large");

    const auto& blocks = can_data_frame_block->second;

    std::vector<uint8_t> data(channel_group->GetDataSizeBytes(), 0);
    if(data.size() != CAN_RECORD_DATA_SIZE)
        throw std::runtime_error("Unexpected CAN data frame record size");

    // Timestamp
    std::memcpy(data.data() + CAN_TIMESTAMP_OFFSET, &time, sizeof(time));

    // CAN_DataFrame.BusChannel (2 bits) | CAN_DataFrame.ID (29 bits) | CAN_DataFrame.IDE (1 bit)
    constexpr uint32_t bus_channel = 0;
    const uint32_t frame_id = can_frame.id & (can_frame.is_extended ? CAN_ID_EXTENDED_MASK : CAN_ID_STANDARD_MASK);
    const uint32_t frame_ide = can_frame.is_extended ? 1U : 0U;

    const uint32_t id_pack = (bus_channel & 0x3U) | (frame_id << 2) | (frame_ide << 31);
    std::memcpy(data.data() + CAN_ID_OFFSET, &id_pack, sizeof(id_pack));

    // CAN_DataFrame.Dir (1 bit) | CAN_DataFrame.DataLength (7 bits)
    const uint32_t frame_dir = can_frame.is_transmit ? 1U : 0U;
    const auto frame_data_length = static_cast<uint32_t>(can_frame.data.size());

    const auto dir_length_pack = static_cast<uint8_t>((frame_dir & 0x1U) | ((frame_data_length & 0x7FU) << 1));
    std::memcpy(data.data() + CAN_DIR_LENGTH_OFFSET, &dir_length_pack, sizeof(dir_length_pack));

    // CAN_DataFrame.EDL (1 bit) | CAN_DataFrame.BRS (1 bit) | CAN_DataFrame.DLC (4 bits)
    const uint32_t frame_edl = can_frame.is_can_fd ? 1U : 0U;
    const uint32_t frame_brs = can_frame.is_bitrate_switch ? 1U : 0U;
    const uint32_t frame_dlc = ToCanDlc(can_frame.data.size());

    const auto edl_brs_dlc_pack = static_cast<uint8_t>(
        (frame_edl & 0x1U) | ((frame_brs & 0x1U) << 1) | ((frame_dlc & 0xFU) << 2));
    std::memcpy(data.data() + CAN_EDL_BRS_DLC_OFFSET, &edl_brs_dlc_pack, sizeof(edl_brs_dlc_pack));

    // CAN_DataFrame.DataBytes, offset of the VLSD record written right after this one
    auto vlsd_offset = blocks.raw_data_vlsd_data_record->GetOffsetData();
    if(CAN_DATA_BYTES_OFFSET + vlsd_offset.size() > data.size())
        throw std::runtime_error("VLSD offset does not fit into the CAN data frame record");

    std::memcpy(data.data() + CAN_DATA_BYTES_OFFSET, vlsd_offset.data(), vlsd_offset.size());

    auto bytes_written = blocks.header_data_record->WriteRawToStream(stream, data);
    bytes_written += blocks.raw_data_vlsd_data_record->WriteToStream(stream, can_frame.data);

    data_bytes_written_ += bytes_written;

    return bytes_written;
}

uint64_t Mdf4File::FinalizeToStream(std::streambuf& stream) {
    if(!is_header_written_)
        throw std::runtime_error("File header has not been written yet");

    auto data_block = data_group_->GetDataBlock();
    data_block->SetDataSizeBytes(data_bytes_written_);

    auto bytes_written = data_block->RewriteToStream(stream);

    for(auto& channel_group : channel_groups_)
        bytes_written += channel_group->RewriteToStream(stream);

    id_block_->SetFinalized(true);
    bytes_written += id_block_->RewriteToStream(stream);

    if(stream.pubseekoff(0, std::ios_base::end, std::ios_base::out)
        == std::streambuf::pos_type(std::streambuf::off_type(-1))) {

        throw std::ios_base::failure("Failed to seek back to the end of the stream.");
    }

    stream.pubsync();
    is_finalized_ = true;

    return bytes_written;
}

} // namespace eerie_leap::subsys::mdf
