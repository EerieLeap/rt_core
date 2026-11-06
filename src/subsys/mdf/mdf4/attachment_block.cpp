#include <algorithm>
#include <cstring>
#include <utility>

#include "attachment_block.h"

namespace eerie_leap::subsys::mdf::mdf4 {

AttachmentBlock::AttachmentBlock() :
    BlockBase("AT") {}

size_t AttachmentBlock::GetPaddedDataSize() const {
    return (data_.size() + 7) & ~static_cast<size_t>(7);
}

void AttachmentBlock::SetFileName(std::shared_ptr<TextBlock> file_name) {
    links_.SetLink(LinkType::FileName, std::move(file_name));
}

void AttachmentBlock::SetMimeType(std::shared_ptr<TextBlock> mime_type) {
    links_.SetLink(LinkType::MimeType, std::move(mime_type));
}

void AttachmentBlock::SetComment(std::shared_ptr<MetadataBlock> comment) {
    links_.SetLink(LinkType::MetadataComment, std::move(comment));
}

void AttachmentBlock::SetData(std::vector<uint8_t> data) {
    data_ = std::move(data);
}

void AttachmentBlock::LinkBlock(std::shared_ptr<AttachmentBlock> next) {
    auto* last = this;
    while(const auto following = last->links_.GetLink(LinkType::AttachmentNext))
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast): the chain holds only AttachmentBlocks (LinkBlock is its only writer)
        last = static_cast<AttachmentBlock*>(following.get());

    last->links_.SetLink(LinkType::AttachmentNext, std::move(next));
}

uint64_t AttachmentBlock::GetBlockSize() const {
    return GetBaseSize() + FIXED_DATA_SIZE + GetPaddedDataSize();
}

std::unique_ptr<uint8_t[]> AttachmentBlock::Serialize() const {
    auto buffer = SerializeBase();
    uint64_t offset = GetBaseSize();

    const uint16_t flags = FLAG_EMBEDDED;
    std::memcpy(buffer.get() + offset, &flags, sizeof(flags));
    offset += sizeof(flags);

    offset += 2;  // at_creator_index: 0
    offset += 4;  // reserved
    offset += 16; // at_md5_checksum: not valid, left zero

    const uint64_t size = data_.size();
    std::memcpy(buffer.get() + offset, &size, sizeof(size)); // at_original_size
    offset += sizeof(size);
    std::memcpy(buffer.get() + offset, &size, sizeof(size)); // at_embedded_size
    offset += sizeof(size);

    std::ranges::copy(data_, buffer.get() + offset); // the padding stays zero

    return buffer;
}

} // namespace eerie_leap::subsys::mdf::mdf4
