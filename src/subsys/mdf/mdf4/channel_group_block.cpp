#include <cstring>
#include <set>
#include <stdexcept>
#include <utility>

#include "text_block.h"

#include "channel_group_block.h"

namespace eerie_leap::subsys::mdf::mdf4 {

ChannelGroupBlock::ChannelGroupBlock(uint8_t record_id_size_bytes, uint64_t record_id) :
    BlockBase("CG"),
    record_id_(record_id),
    record_id_size_bytes_(record_id_size_bytes) {

    if(!std::set<uint8_t>{ 0, 1, 2, 4, 8 }.contains(record_id_size_bytes_))
        throw std::runtime_error("Invalid record ID size bytes");

    if(record_id_size_bytes_ < 8 && record_id_ >= (1ULL << (record_id_size_bytes_ * 8)))
        throw std::runtime_error("Record ID does not fit into the data group record ID size");

    cycle_count_ = 0;
    flags_ = 0;
    path_separator_ = 0;
    data_bytes_ = 0;
    invalidation_bytes_ = 0;
    vlsd_data_bytes_ = 0;
}

// Destroying the next block here would destroy the one after it from its destructor, one nested
// call per block of the chain; each block this chain still owns gives up its next link first.
ChannelGroupBlock::~ChannelGroupBlock() {
    auto next = links_.TakeLink(LinkType::ChannelGroupNext);
    while(next && next.use_count() == 1)
        next = static_cast<ChannelGroupBlock&>(*next).links_.TakeLink(LinkType::ChannelGroupNext);
}

uint16_t ChannelGroupBlock::GetFlags() const {
    return flags_;
}

uint64_t ChannelGroupBlock::GetRecordId() const {
    return record_id_;
}

uint8_t ChannelGroupBlock::GetRecordIdSizeBytes() const {
    return record_id_size_bytes_;
}

std::array<uint8_t, 8> ChannelGroupBlock::GetRecordIdData() const {
    // The constructor rejected an ID wider than its size, so the bytes past the size are zero.
    std::array<uint8_t, 8> buffer{};
    std::memcpy(buffer.data(), &record_id_, sizeof(record_id_));

    return buffer;
}

uint32_t ChannelGroupBlock::GetDataSizeBytes() const {
    return data_bytes_;
}

uint32_t ChannelGroupBlock::GetInvalidationBytes() const {
    return invalidation_bytes_;
}

uint64_t ChannelGroupBlock::GetCycleCount() const {
    return cycle_count_;
}

uint64_t ChannelGroupBlock::GetVlsdDataSizeBytes() const {
    return vlsd_data_bytes_;
}

void ChannelGroupBlock::IncrementCycleCount() {
    cycle_count_++;
}

void ChannelGroupBlock::AddVlsdDataBytes(uint64_t bytes) {
    vlsd_data_bytes_ += bytes;
}

void ChannelGroupBlock::ResetCounters() {
    cycle_count_ = 0;
    vlsd_data_bytes_ = 0;
}

std::vector<std::shared_ptr<ChannelBlock>> ChannelGroupBlock::GetChannels() const {
    std::vector<std::shared_ptr<ChannelBlock>> channels;

    auto next_channel = std::dynamic_pointer_cast<ChannelBlock>(links_.GetLink(LinkType::ChannelFirst));
    while(next_channel) {
        channels.push_back(next_channel);
        next_channel = next_channel->GetLinkedChannel();
    }

    return channels;
}

void ChannelGroupBlock::AddChannel(std::shared_ptr<ChannelBlock> channel) {
    if(channel->GetDataSizeBytes() == 0)
        throw std::runtime_error("Channel bit count must be set before adding it to a channel group");

    channel->SetOffsetBytes(data_bytes_);
    data_bytes_ += channel->GetDataSizeBytes();

    if(links_.GetLink(LinkType::ChannelFirst)) {
        const auto first_channel = std::dynamic_pointer_cast<ChannelBlock>(links_.GetLink(LinkType::ChannelFirst));
        first_channel->LinkBlock(std::move(channel));
    } else {
        links_.SetLink(LinkType::ChannelFirst, channel);
    }
}

void ChannelGroupBlock::LinkBlock(std::shared_ptr<ChannelGroupBlock> next_block) {
    auto* last = this;
    while(const auto next = last->links_.GetLink(LinkType::ChannelGroupNext))
        last = static_cast<ChannelGroupBlock*>(next.get());

    last->links_.SetLink(LinkType::ChannelGroupNext, std::move(next_block));
}

void ChannelGroupBlock::UnlinkNext() {
    links_.SetLink(LinkType::ChannelGroupNext, nullptr);
}

void ChannelGroupBlock::AddSourceInformation(const std::shared_ptr<SourceInformationBlock>& source_information) {
    links_.SetLink(LinkType::SourceInformation, source_information);
}

void ChannelGroupBlock::SetFlags(uint16_t flags) {
    if((flags & std::to_underlying(Flag::VlsdChannel)) && invalidation_bytes_ != 0)
        throw std::runtime_error("A VLSD channel group has no invalidation bytes");

    flags_ = flags;
}

void ChannelGroupBlock::SetInvalidationBytes(uint32_t invalidation_bytes) {
    if(flags_ & std::to_underlying(Flag::VlsdChannel))
        throw std::runtime_error("A VLSD channel group has no invalidation bytes");

    invalidation_bytes_ = invalidation_bytes;
}

void ChannelGroupBlock::SetPathSeparator(uint16_t path_separator) {
    path_separator_ = path_separator;
}

void ChannelGroupBlock::SetName(const std::shared_ptr<TextBlock>& name) {
    links_.SetLink(LinkType::TextAcquisitionName, name);
}

void ChannelGroupBlock::SetComment(std::shared_ptr<MetadataBlock> comment) {
    links_.SetLink(LinkType::MetadataComment, std::move(comment));
}

uint64_t ChannelGroupBlock::GetBlockSize() const {
    return GetBaseSize() + 8 + 8 + 2 + 2 + 4 + 4 + 4;
}

std::unique_ptr<uint8_t[]> ChannelGroupBlock::Serialize() const {
    auto buffer = SerializeBase();
    uint64_t offset = GetBaseSize();

    std::memcpy(buffer.get() + offset, &record_id_, sizeof(record_id_));
    offset += sizeof(record_id_);

    std::memcpy(buffer.get() + offset, &cycle_count_, sizeof(cycle_count_));
    offset += sizeof(cycle_count_);

    std::memcpy(buffer.get() + offset, &flags_, sizeof(flags_));
    offset += sizeof(flags_);

    std::memcpy(buffer.get() + offset, &path_separator_, sizeof(path_separator_));
    offset += sizeof(path_separator_);

    offset += 4; // reserved_1_

    // A VLSD group reports the total size of all its values as a UINT64 split over both fields.
    const bool is_vlsd = (flags_ & std::to_underlying(Flag::VlsdChannel)) != 0;
    const auto data_bytes = is_vlsd ? static_cast<uint32_t>(vlsd_data_bytes_ & 0xFFFFFFFFULL) : data_bytes_;
    const auto invalidation_bytes = is_vlsd ? static_cast<uint32_t>(vlsd_data_bytes_ >> 32) : invalidation_bytes_;

    std::memcpy(buffer.get() + offset, &data_bytes, sizeof(data_bytes));
    offset += sizeof(data_bytes);

    std::memcpy(buffer.get() + offset, &invalidation_bytes, sizeof(invalidation_bytes));
    offset += sizeof(invalidation_bytes);

    return buffer;
}

} // namespace eerie_leap::subsys::mdf::mdf4
