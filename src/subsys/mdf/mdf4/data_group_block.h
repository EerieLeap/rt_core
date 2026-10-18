#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "block_base.h"
#include "channel_group_block.h"
#include "data_block.h"
#include "subsys/mdf/utilities/block_links.h"

namespace eerie_leap::subsys::mdf::mdf4 {

class DataGroupBlock : public BlockBase {
private:
    enum class LinkType : int {
        DataGroupNext = 0,
        ChannelGroupFirst,
        Data,
        MetadataComment
    };

    utilities::BlockLinks<LinkType, 4> links_;

    uint8_t record_id_size_bytes_; // 1 byte, Length of record ID bytes
    // uint8_t reserved_1_[7];          // 7 bytes, Reserved

    // Where the next channel group is linked; owned by the chain.
    ChannelGroupBlock* last_channel_group_{ nullptr };

public:
    explicit DataGroupBlock(uint8_t record_id_size_bytes);
    ~DataGroupBlock() override;
    DataGroupBlock(const DataGroupBlock&) = delete;
    DataGroupBlock& operator=(const DataGroupBlock&) = delete;
    DataGroupBlock(DataGroupBlock&&) = delete;
    DataGroupBlock& operator=(DataGroupBlock&&) = delete;

    uint8_t GetRecordIdSizeBytes() const;
    std::shared_ptr<DataBlock> GetDataBlock() const;

    uint64_t GetBlockSize() const override;
    std::unique_ptr<uint8_t[]> Serialize() const override;
    const IBlockLinks* GetBlockLinks() const override {
        return &links_;
    }
    std::shared_ptr<ISerializableBlock> GetNext() const override {
        return links_.GetLink(LinkType::DataGroupNext);
    }
    std::vector<std::shared_ptr<ISerializableBlock>> GetChildren() const override {
        return { links_.GetLink(LinkType::MetadataComment), links_.GetLink(LinkType::ChannelGroupFirst),
            links_.GetLink(LinkType::Data) };
    }

    void AddChannelGroup(std::shared_ptr<ChannelGroupBlock> channel_group);
    /** Drops the chain; the file calls it when it frees its groups, so the tail never dangles. */
    void ClearChannelGroups();
    void LinkBlock(std::shared_ptr<DataGroupBlock> next_block);
};

} // namespace eerie_leap::subsys::mdf::mdf4
