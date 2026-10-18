#include <cstring>
#include <set>
#include <stdexcept>
#include <utility>

#include "data_group_block.h"

namespace eerie_leap::subsys::mdf::mdf4 {

DataGroupBlock::DataGroupBlock(uint8_t record_id_size_bytes) :
    BlockBase("DG"),
    record_id_size_bytes_(record_id_size_bytes),
{

    if(!std::set<uint8_t>{ 0, 1, 2, 4, 8 }.contains(record_id_size_bytes_))
        throw std::runtime_error("Invalid record ID size bytes");

    links_.SetLink(LinkType::Data, std::make_shared<DataBlock>());
}

// See ~ChannelGroupBlock(): no destructor recursion along the chain.
DataGroupBlock::~DataGroupBlock() {
    auto next = links_.TakeLink(LinkType::DataGroupNext);
    while(next && next.use_count() == 1)
        next = static_cast<DataGroupBlock&>(*next).links_.TakeLink(LinkType::DataGroupNext);
}

uint8_t DataGroupBlock::GetRecordIdSizeBytes() const {
    return record_id_size_bytes_;
}

std::shared_ptr<DataBlock> DataGroupBlock::GetDataBlock() const {
    return std::dynamic_pointer_cast<DataBlock>(links_.GetLink(LinkType::Data));
}

// Linking from the last group keeps adding a group constant time; a file has hundreds of them.
void DataGroupBlock::AddChannelGroup(std::shared_ptr<ChannelGroupBlock> channel_group) {
    if(!channel_group)
        throw std::invalid_argument("Channel group cannot be null");

    auto* last_channel_group = channel_group.get();

    if(last_channel_group_ != nullptr)
        last_channel_group_->LinkBlock(std::move(channel_group));
    else
        links_.SetLink(LinkType::ChannelGroupFirst, std::move(channel_group));

    last_channel_group_ = last_channel_group;
}

void DataGroupBlock::ClearChannelGroups() {
    links_.SetLink(LinkType::ChannelGroupFirst, nullptr);
    last_channel_group_ = nullptr;
}

void DataGroupBlock::LinkBlock(std::shared_ptr<DataGroupBlock> next_block) {
    auto* last = this;
    while(const auto next = last->links_.GetLink(LinkType::DataGroupNext))
        last = static_cast<DataGroupBlock*>(next.get());

    last->links_.SetLink(LinkType::DataGroupNext, std::move(next_block));
}

uint64_t DataGroupBlock::GetBlockSize() const {
    return GetBaseSize() + 1 + 7;
}

std::unique_ptr<uint8_t[]> DataGroupBlock::Serialize() const {
    auto buffer = SerializeBase();
    uint64_t offset = GetBaseSize();

    std::memcpy(buffer.get() + offset, &record_id_size_bytes_, sizeof(record_id_size_bytes_));
    offset += sizeof(record_id_size_bytes_);

    offset += 7; // reserved_1_

    return buffer;
}

} // namespace eerie_leap::subsys::mdf::mdf4
