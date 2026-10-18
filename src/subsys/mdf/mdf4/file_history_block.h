#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "block_base.h"
#include "metadata_block.h"
#include "subsys/mdf/utilities/block_links.h"

namespace eerie_leap::subsys::mdf::mdf4 {

class FileHistoryBlock : public BlockBase {
private:
    enum class LinkType : int {
        FileHistoryNext = 0,
        MetadataComment
    };

    utilities::BlockLinks<LinkType, 2> links_;

    uint64_t time_ns_{ 0 };       // 8 bytes, Absolute time in ns since midnight Jan 1st, 1970
    int16_t tz_offset_min_{ 0 };  // 2 bytes, Timezone offset in minutes
    int16_t dst_offset_min_{ 0 }; // 2 bytes, Daylight saving time (DST) offset in minutes
    uint8_t time_flags_{ 0 };     // 1 bytes, Time flags
    // uint8_t reserved_1_[3];          // 3 bytes, Reserved

public:
    FileHistoryBlock();
    ~FileHistoryBlock() override;
    FileHistoryBlock(const FileHistoryBlock&) = delete;
    FileHistoryBlock& operator=(const FileHistoryBlock&) = delete;
    FileHistoryBlock(FileHistoryBlock&&) = delete;
    FileHistoryBlock& operator=(FileHistoryBlock&&) = delete;

    void SetTimeNs(uint64_t time_ns);
    /** Rebuilds the FHcomment with the tool fields XML escaped. */
    void SetTool(std::string_view tool_id, std::string_view tool_vendor, std::string_view tool_version);

    uint64_t GetBlockSize() const override;
    std::unique_ptr<uint8_t[]> Serialize() const override;
    const IBlockLinks* GetBlockLinks() const override {
        return &links_;
    }
    std::shared_ptr<ISerializableBlock> GetNext() const override {
        return links_.GetLink(LinkType::FileHistoryNext);
    }
    std::vector<std::shared_ptr<ISerializableBlock>> GetChildren() const override {
        return { links_.GetLink(LinkType::MetadataComment) };
    }

    void LinkBlock(std::shared_ptr<FileHistoryBlock> next_block);
};

} // namespace eerie_leap::subsys::mdf::mdf4
