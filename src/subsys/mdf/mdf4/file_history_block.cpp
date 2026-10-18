#include <cstring>
#include <memory>
#include <utility>

#include "file_history_block.h"

namespace eerie_leap::subsys::mdf::mdf4 {

FileHistoryBlock::FileHistoryBlock() :
    BlockBase("FH") {
    time_ns_ = 0;
    tz_offset_min_ = 0;
    dst_offset_min_ = 0;
    time_flags_ = 0;

    links_.SetLink(LinkType::MetadataComment, std::make_shared<MetadataBlock>());
    SetTool("none", "Eerie Leap", "0.0.0");
}

// See ~ChannelGroupBlock(): no destructor recursion along the chain.
FileHistoryBlock::~FileHistoryBlock() {
    auto next = links_.TakeLink(LinkType::FileHistoryNext);
    while(next && next.use_count() == 1)
        next = static_cast<FileHistoryBlock&>(*next).links_.TakeLink(LinkType::FileHistoryNext);
}

void FileHistoryBlock::SetTool(std::string_view tool_id, std::string_view tool_vendor, std::string_view tool_version) {
    const auto metadata_comment_block = std::dynamic_pointer_cast<MetadataBlock>(
        links_.GetLink(LinkType::MetadataComment)
    );
    metadata_comment_block->SetText(
        GetId(),
        "<TX>created</TX>"
        "<tool_id>"
            + MetadataBlock::EscapeXml(tool_id)
            + "</tool_id>"
              "<tool_vendor>"
            + MetadataBlock::EscapeXml(tool_vendor)
            + "</tool_vendor>"
              "<tool_version>"
            + MetadataBlock::EscapeXml(tool_version) + "</tool_version>"
    );
}

void FileHistoryBlock::SetTimeNs(uint64_t time_ns) {
    time_ns_ = time_ns;
}

void FileHistoryBlock::LinkBlock(std::shared_ptr<FileHistoryBlock> next_block) {
    auto* last = this;
    while(const auto next = last->links_.GetLink(LinkType::FileHistoryNext))
        last = static_cast<FileHistoryBlock*>(next.get());

    last->links_.SetLink(LinkType::FileHistoryNext, std::move(next_block));
}

uint64_t FileHistoryBlock::GetBlockSize() const {
    return GetBaseSize() + 8 + 2 + 2 + 1 + 3;
}

std::unique_ptr<uint8_t[]> FileHistoryBlock::Serialize() const {
    auto buffer = SerializeBase();
    uint64_t offset = GetBaseSize();

    std::memcpy(buffer.get() + offset, &time_ns_, sizeof(time_ns_));
    offset += sizeof(time_ns_);

    std::memcpy(buffer.get() + offset, &tz_offset_min_, sizeof(tz_offset_min_));
    offset += sizeof(tz_offset_min_);

    std::memcpy(buffer.get() + offset, &dst_offset_min_, sizeof(dst_offset_min_));
    offset += sizeof(dst_offset_min_);

    std::memcpy(buffer.get() + offset, &time_flags_, sizeof(time_flags_));
    offset += sizeof(time_flags_);

    offset += 3; // reserved_1_

    return buffer;
}

} // namespace eerie_leap::subsys::mdf::mdf4
