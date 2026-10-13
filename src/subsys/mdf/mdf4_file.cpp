#include <algorithm>
#include <array>
#include <cstring>
#include <ios>
#include <stdexcept>
#include <string>
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

using mdf4::CanRecordWriter;
using mdf4::RecordWriter;

namespace {

// One byte after the data bytes holds the invalidation bits of a group, so eight channels can have one.
constexpr uint32_t INVALIDATION_BYTES = 1;
constexpr uint32_t MAX_INVALIDATION_BITS = INVALIDATION_BYTES * 8;

} // namespace

Mdf4File::Mdf4File(Mdf4FileOptions options)
    : options_(options), is_header_written_(false), is_finalized_(false) {

    const auto id_size = options_.record_id_size_bytes;
    if(id_size != 1 && id_size != 2 && id_size != 4 && id_size != 8)
        throw std::invalid_argument("Record ID size must be 1, 2, 4 or 8 bytes");

    if(options_.time_type != MdfDataType::Float32 && options_.time_type != MdfDataType::Float64)
        throw std::invalid_argument("Master time channel must be Float32 or Float64");

    if(options_.vlsd_offset_size_bytes != 4 && options_.vlsd_offset_size_bytes != 8)
        throw std::invalid_argument("VLSD offset size must be 4 or 8 bytes");

    id_block_ = std::make_unique<mdf4::IdBlock>();
    header_block_ = std::make_unique<mdf4::HeaderBlock>();

    data_group_ = std::make_shared<mdf4::DataGroupBlock>(options_.record_id_size_bytes);
    header_block_->AddDataGroup(data_group_);
}

// A CAN group's DataBytes channel owns its VLSD group, which owns every later group through its next
// link: a cycle no owner would ever free. Unlinking the chain while channel_groups_ still owns each
// group breaks it, and leaves no chain for a destructor to walk.
Mdf4File::~Mdf4File() {
    for(auto& channel_group : channel_groups_)
        channel_group->UnlinkNext();

    // The data group can outlive the file through GetDataGroup(); it must not keep a chain it no
    // longer owns whole, nor a tail pointer into a freed group.
    data_group_->ClearChannelGroups();
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

void Mdf4File::EnsureStructureEditable() const {
    if(is_header_written_)
        throw std::runtime_error("The file structure cannot change after the header was written");
}

std::shared_ptr<mdf4::MetadataBlock> Mdf4File::CreateCommentBlock(
    const std::string& block_id, std::string_view text, std::span<const MdfProperty> properties) {

    EnsureStructureEditable();

    auto comment = std::make_shared<mdf4::MetadataBlock>();
    comment->SetComment(block_id, std::string(text), properties);

    return comment;
}

void Mdf4File::SetHeaderComment(std::string_view text, std::span<const MdfProperty> properties) {
    header_block_->SetComment(CreateCommentBlock(header_block_->GetId(), text, properties));
}

void Mdf4File::SetFileHistory(std::string_view tool_id, std::string_view tool_vendor, std::string_view tool_version) {
    EnsureStructureEditable();
    header_block_->GetFileHistory()->SetTool(tool_id, tool_vendor, tool_version);
}

void Mdf4File::SetChannelComment(const std::shared_ptr<mdf4::ChannelBlock>& channel, std::string_view text) {
    if(!channel)
        throw std::invalid_argument("Channel cannot be null");

    channel->SetComment(CreateCommentBlock(channel->GetId(), text));
}

void Mdf4File::SetChannelGroupComment(const std::shared_ptr<mdf4::ChannelGroupBlock>& channel_group, std::string_view text) {
    if(!channel_group)
        throw std::invalid_argument("Channel group cannot be null");

    channel_group->SetComment(CreateCommentBlock(channel_group->GetId(), text));
}

std::shared_ptr<mdf4::ChannelGroupBlock> Mdf4File::CreateChannelGroupBlock(uint64_t record_id) {
    EnsureStructureEditable();

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
    auto channel_group = CreateTimeChannelGroup(record_id, name);
    record_channel_groups_.push_back(channel_group);

    return channel_group;
}

std::shared_ptr<mdf4::ChannelGroupBlock> Mdf4File::CreateTimeChannelGroup(uint64_t record_id, const std::string& name) {
    auto channel_group = CreateChannelGroupBlock(record_id);
    channel_group->SetName(GetOrCreateTextBlock(name));

    auto channel_data_type = MdfHelpers::ToMdf4ChannelDataType(options_.time_type);
    auto channel_time = std::make_shared<mdf4::ChannelBlock>(
        mdf4::ChannelBlock::Type::Master,
        mdf4::ChannelBlock::SyncType::Time,
        channel_data_type.data_type,
        channel_data_type.bit_count);
    channel_time->SetName(GetOrCreateTextBlock("Timestamp"));
    channel_time->SetUnit(GetOrCreateTextBlock("s"));
    channel_group->AddChannel(channel_time);

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

    auto channel_group = CreateTimeChannelGroup(record_id, name);
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

    // Member offsets are relative to the record data, so they start after the master time channel.
    const uint32_t frame_offset = channel_group->GetDataSizeBytes();
    const uint32_t frame_size = CanRecordWriter::FrameSizeBytes(options_.vlsd_offset_size_bytes);

    auto can_data_frame_channel = CreateChannelBlock(MdfDataType::ByteArray, "CAN_DataFrame");
    can_data_frame_channel->SetFlags(std::to_underlying(mdf4::ChannelBlock::Flag::BusEvent));
    can_data_frame_channel->SetBitCount(frame_size * 8);
    channel_group->AddChannel(can_data_frame_channel);

    auto add_member = [&](const std::string& name, uint32_t byte_offset, uint8_t bit_offset, uint32_t bit_count) {
        auto member = CreateChannelBlock(MdfDataType::Uint32, name);
        member->SetFlags(std::to_underlying(mdf4::ChannelBlock::Flag::BusEvent));
        member->SetOffsetBytes(frame_offset + byte_offset);
        member->SetOffsetBits(bit_offset);
        member->SetBitCount(bit_count);

        return member;
    };

    // BusChannel numbers real buses from 1; 0 means "not specified".
    auto can_data_frame_bus_channel = add_member("CAN_DataFrame.BusChannel", CanRecordWriter::BUS_CHANNEL_OFFSET, 0, 8);
    can_data_frame_channel->SetArrayBlock(can_data_frame_bus_channel);

    can_data_frame_bus_channel->LinkBlock(add_member("CAN_DataFrame.ID", CanRecordWriter::ID_OFFSET, 0, 29));

    // IDE (Identifier Extension) | 0 - 11 bit ID, 1 - 29 bit ID; bit 31 of the ID word
    can_data_frame_bus_channel->LinkBlock(add_member("CAN_DataFrame.IDE", CanRecordWriter::ID_OFFSET + CanRecordWriter::IDE_BIT / 8, CanRecordWriter::IDE_BIT % 8, 1));

    // DLC (Data Length Code)
    can_data_frame_bus_channel->LinkBlock(add_member("CAN_DataFrame.DLC", CanRecordWriter::FLAGS_OFFSET, CanRecordWriter::DLC_BIT, 4));

    // Dir (Direction) | 0 - Receive, 1 - Transmit
    can_data_frame_bus_channel->LinkBlock(add_member("CAN_DataFrame.Dir", CanRecordWriter::FLAGS_OFFSET, CanRecordWriter::DIR_BIT, 1));

    // EDL (Extended Data Length) | 0 - Standard CAN, 1 - CAN FD
    can_data_frame_bus_channel->LinkBlock(add_member("CAN_DataFrame.EDL", CanRecordWriter::FLAGS_OFFSET, CanRecordWriter::EDL_BIT, 1));

    // BRS (Bit Rate Switch)
    can_data_frame_bus_channel->LinkBlock(add_member("CAN_DataFrame.BRS", CanRecordWriter::FLAGS_OFFSET, CanRecordWriter::BRS_BIT, 1));

    // ESI (Error State Indicator); always 0 because CanFrame carries no ESI field.
    can_data_frame_bus_channel->LinkBlock(add_member("CAN_DataFrame.ESI", CanRecordWriter::FLAGS_OFFSET, CanRecordWriter::ESI_BIT, 1));

    can_data_frame_bus_channel->LinkBlock(add_member("CAN_DataFrame.DataLength", CanRecordWriter::DATA_LENGTH_OFFSET, 0, 8));

    // VLSD data block offset
    auto can_data_frame_data_bytes_channel = CreateChannelBlock(MdfDataType::ByteArray, "CAN_DataFrame.DataBytes");
    can_data_frame_data_bytes_channel->SetType(mdf4::ChannelBlock::Type::VariableLength);
    can_data_frame_data_bytes_channel->SetFlags(std::to_underlying(mdf4::ChannelBlock::Flag::BusEvent));
    can_data_frame_data_bytes_channel->SetOffsetBytes(frame_offset + CanRecordWriter::DATA_BYTES_OFFSET);
    can_data_frame_data_bytes_channel->SetBitCount(options_.vlsd_offset_size_bytes * 8U);
    can_data_frame_data_bytes_channel->SetSignalDataBlock(vlsd_channel_group);
    can_data_frame_bus_channel->LinkBlock(can_data_frame_data_bytes_channel);

    if(channel_group->GetDataSizeBytes() != frame_offset + frame_size)
        throw std::runtime_error("Unexpected CAN data frame record size");

    can_channel_groups_.push_back(CanChannelGroups{
        .channel_group = channel_group,
        .vlsd_channel_group = vlsd_channel_group
    });

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
    const std::shared_ptr<mdf4::ChannelGroupBlock>& channel_group,
    MdfDataType data_type,
    const std::string& name,
    const std::string& unit,
    bool with_invalidation_bit) {

    EnsureStructureEditable();

    if(std::ranges::find(record_channel_groups_, channel_group) == record_channel_groups_.end())
        throw std::runtime_error("Channels can only be added to a plain channel group");

    const auto channels = channel_group->GetChannels();
    if(channels.size() + 1 > RecordWriter::kMaxFields) {
        throw std::runtime_error("A record of more than " + std::to_string(RecordWriter::kMaxFields)
            + " channels does not fit the record writer");
    }

    const auto invalidation_bit_count = static_cast<uint32_t>(std::ranges::count_if(channels,
        [](const auto& channel) { return channel->HasInvalidationBit(); }));

    if(with_invalidation_bit && invalidation_bit_count >= MAX_INVALIDATION_BITS) {
        throw std::runtime_error("A channel group has invalidation bits for at most "
            + std::to_string(MAX_INVALIDATION_BITS) + " channels");
    }

    const uint32_t invalidation_bytes = with_invalidation_bit
        ? std::max(channel_group->GetInvalidationBytes(), INVALIDATION_BYTES)
        : channel_group->GetInvalidationBytes();

    const uint64_t record_size = channel_group->GetRecordIdSizeBytes()
        + static_cast<uint64_t>(channel_group->GetDataSizeBytes())
        + MdfHelpers::ToMdf4ChannelDataType(data_type).bit_count / 8
        + invalidation_bytes;

    if(record_size > RecordWriter::kMaxRecordBytes) {
        throw std::runtime_error("A record of " + std::to_string(record_size) + " bytes exceeds the "
            + std::to_string(RecordWriter::kMaxRecordBytes) + " byte limit of the record writer");
    }

    auto channel = CreateChannelBlock(data_type, name, unit);
    if(with_invalidation_bit) {
        channel->SetInvalidationBit(invalidation_bit_count);
        channel_group->SetInvalidationBytes(invalidation_bytes);
    }
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

    is_header_written_ = false;
    is_finalized_ = false;

    // Before anything is written: a layout the writers refuse leaves the stream untouched.
    BuildRecordWriters();

    for(auto& channel_group : channel_groups_)
        channel_group->ResetCounters();

    data_block->SetDataSizeBytes(0);

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

void Mdf4File::BuildRecordWriters() {
    record_writers_.clear();
    can_record_writers_.clear();

    record_writers_.reserve(record_channel_groups_.size());
    for(const auto& channel_group : record_channel_groups_)
        record_writers_.emplace_back(*channel_group);

    can_record_writers_.reserve(can_channel_groups_.size());
    for(const auto& groups : can_channel_groups_) {
        can_record_writers_.emplace_back(
            *groups.channel_group,
            *groups.vlsd_channel_group,
            options_.time_type,
            options_.vlsd_offset_size_bytes);
    }
}

RecordWriter& Mdf4File::GetRecordWriter(const std::shared_ptr<mdf4::ChannelGroupBlock>& channel_group) {
    if(!is_header_written_)
        throw std::runtime_error("Record writers exist once the header has been written");

    auto writer = std::ranges::find(record_writers_, channel_group.get(), &RecordWriter::GetChannelGroup);
    if(writer == record_writers_.end())
        throw std::runtime_error("Invalid channel group");

    return *writer;
}

CanRecordWriter& Mdf4File::GetCanRecordWriter(const std::shared_ptr<mdf4::ChannelGroupBlock>& channel_group) {
    if(!is_header_written_)
        throw std::runtime_error("Record writers exist once the header has been written");

    auto writer = std::ranges::find(can_record_writers_, channel_group.get(), &CanRecordWriter::GetChannelGroup);
    if(writer == can_record_writers_.end())
        throw std::runtime_error("Invalid channel group");

    return *writer;
}

uint64_t Mdf4File::WriteDataRecordToStream(
    const std::shared_ptr<mdf4::ChannelGroupBlock>& channel_group,
    std::streambuf& stream,
    std::span<const MdfValue> values,
    uint32_t invalid_mask) {

    EnsureRecordsWritable();

    return GetRecordWriter(channel_group).Write(stream, values, invalid_mask);
}

uint64_t Mdf4File::WriteCanbusDataRecordToStream(
        const std::shared_ptr<mdf4::ChannelGroupBlock>& channel_group,
        std::streambuf& stream,
        const CanFrame& can_frame,
        double time,
        uint8_t bus_channel) {

    EnsureRecordsWritable();

    return GetCanRecordWriter(channel_group).Write(stream, can_frame, time, bus_channel);
}

uint64_t Mdf4File::FinalizeToStream(std::streambuf& stream) {
    if(!is_header_written_)
        throw std::runtime_error("File header has not been written yet");

    uint64_t data_bytes_written = 0;
    for(const auto& writer : record_writers_)
        data_bytes_written += writer.GetBytesWritten();
    for(const auto& writer : can_record_writers_)
        data_bytes_written += writer.GetBytesWritten();

    auto data_block = data_group_->GetDataBlock();
    data_block->SetDataSizeBytes(data_bytes_written);

    auto bytes_written = data_block->RewriteToStream(stream);

    for(auto& channel_group : channel_groups_)
        bytes_written += channel_group->RewriteToStream(stream);

    // The identifier is what tells a reader the lengths and counts are final, so they have to be on
    // the medium before it flips: FAT writes the directory entry and the data at sync, not at write.
    if(stream.pubsync() != 0)
        throw std::ios_base::failure("Failed to sync the stream before finalizing the identifier.");

    id_block_->SetFinalized(true);
    bytes_written += id_block_->RewriteToStream(stream);

    if(stream.pubseekoff(0, std::ios_base::end, std::ios_base::out)
        == std::streambuf::pos_type(std::streambuf::off_type(-1))) {

        throw std::ios_base::failure("Failed to seek back to the end of the stream.");
    }

    if(stream.pubsync() != 0)
        throw std::ios_base::failure("Failed to sync the finalized stream.");

    is_finalized_ = true;

    return bytes_written;
}

} // namespace eerie_leap::subsys::mdf
