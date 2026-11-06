#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "block_base.h"
#include "metadata_block.h"
#include "subsys/mdf/utilities/block_links.h"
#include "text_block.h"

namespace eerie_leap::subsys::mdf::mdf4 {

/**
 * An embedded attachment (##AT): a file inside the MDF file. The block owns the bytes, since the
 * header, and so every attachment, is serialized again for each file of a session.
 */
class AttachmentBlock : public BlockBase {
private:
    enum class LinkType : int {
        AttachmentNext = 0,
        FileName,
        MimeType,
        MetadataComment
    };

    static constexpr uint16_t FLAG_EMBEDDED = 0x0001; // bit 2 (MD5 valid) stays clear
    // flags 2, creator index 2, reserved 4, MD5 16, original size 8, embedded size 8
    static constexpr uint64_t FIXED_DATA_SIZE = 2 + 2 + 4 + 16 + 8 + 8;

    utilities::BlockLinks<LinkType, 4> links_;

    std::vector<uint8_t> data_;

    // Zero to seven bytes, so the block length stays a multiple of 8.
    size_t GetPaddedDataSize() const;

public:
    AttachmentBlock();
    ~AttachmentBlock() override = default;

    void SetFileName(std::shared_ptr<TextBlock> file_name);
    void SetMimeType(std::shared_ptr<TextBlock> mime_type);
    void SetComment(std::shared_ptr<MetadataBlock> comment);
    void SetData(std::vector<uint8_t> data);
    size_t GetDataSize() const {
        return data_.size();
    }

    uint64_t GetBlockSize() const override;
    std::unique_ptr<uint8_t[]> Serialize() const override;
    const IBlockLinks* GetBlockLinks() const override {
        return &links_;
    }
    std::shared_ptr<ISerializableBlock> GetNext() const override {
        return links_.GetLink(LinkType::AttachmentNext);
    }
    std::vector<std::shared_ptr<ISerializableBlock>> GetChildren() const override {
        return { links_.GetLink(LinkType::FileName), links_.GetLink(LinkType::MimeType),
            links_.GetLink(LinkType::MetadataComment) };
    }

    /** Appends @p next to the end of the chain. */
    void LinkBlock(std::shared_ptr<AttachmentBlock> next);
};

} // namespace eerie_leap::subsys::mdf::mdf4
