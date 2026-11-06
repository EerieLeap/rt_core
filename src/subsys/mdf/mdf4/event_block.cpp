#include <cstring>
#include <utility>

#include "event_block.h"

namespace eerie_leap::subsys::mdf::mdf4 {

EventBlock::EventBlock() :
    BlockBase("EV") {}

void EventBlock::SetName(std::shared_ptr<TextBlock> name) {
    links_.SetLink(LinkType::TxName, std::move(name));
}

void EventBlock::SetComment(std::shared_ptr<MetadataBlock> comment) {
    links_.SetLink(LinkType::MdComment, std::move(comment));
}

uint64_t EventBlock::GetBlockSize() const {
    return GetBaseSize() + 1 + 1 + 1 + 1 + 1 + 3 + 4 + 2 + 2 + 8 + 8;
}

std::unique_ptr<uint8_t[]> EventBlock::Serialize() const {
    auto buffer = SerializeBase();
    uint64_t offset = GetBaseSize();

    buffer[offset++] = static_cast<uint8_t>(type_);
    buffer[offset++] = static_cast<uint8_t>(sync_type_);
    buffer[offset++] = static_cast<uint8_t>(range_type_);
    buffer[offset++] = static_cast<uint8_t>(cause_);
    buffer[offset++] = flags_;

    offset += 3; // reserved_1_

    std::memcpy(buffer.get() + offset, &scope_count_, sizeof(scope_count_));
    offset += sizeof(scope_count_);

    std::memcpy(buffer.get() + offset, &attachment_count_, sizeof(attachment_count_));
    offset += sizeof(attachment_count_);

    std::memcpy(buffer.get() + offset, &creator_index_, sizeof(creator_index_));
    offset += sizeof(creator_index_);

    std::memcpy(buffer.get() + offset, &sync_base_value_, sizeof(sync_base_value_));
    offset += sizeof(sync_base_value_);

    std::memcpy(buffer.get() + offset, &sync_factor_, sizeof(sync_factor_));

    return buffer;
}

} // namespace eerie_leap::subsys::mdf::mdf4
