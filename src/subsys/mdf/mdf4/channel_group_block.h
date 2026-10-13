#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "subsys/mdf/utilities/block_links.h"
#include "metadata_block.h"
#include "channel_block.h"
#include "source_information_block.h"
#include "block_base.h"

namespace eerie_leap::subsys::mdf::mdf4 {

class ChannelGroupBlock : public BlockBase {
public:
    enum class Flag: uint16_t {
        Default = 0x0000,
        VlsdChannel = 0x0001,
        BusEvent = 0x0002,
        PlainBusEvent = 0x0004,
        RemoteMaster = 0x0008,
        EventSignal = 0x00010
    };

private:
    enum class LinkType: int {
        ChannelGroupNext = 0,
        ChannelFirst,
        TextAcquisitionName,
        SourceInformation,
        SampleReductionFirst,
        MetadataComment
    };

    utilities::BlockLinks<LinkType, 6> links_;

    uint64_t record_id_;                 // 8 bytes, Record ID
    uint64_t cycle_count_;               // 8 bytes, Number of cycles
    uint16_t flags_;                     // 2 bytes, Flags
    uint16_t path_separator_;            // 2 bytes, Path separator
    // uint8_t reserved_1_[4];           // 4 bytes, Reserved
    uint32_t data_bytes_;                // 4 bytes, Number of bytes in record used for sample values
    uint32_t invalidation_bytes_;        // 4 bytes, Number of bytes in record used for invalidation bits

    uint8_t record_id_size_bytes_;
    // For a VLSD group data_bytes_/invalidation_bytes_ instead hold this total as a UINT64.
    uint64_t vlsd_data_bytes_;

public:
    ChannelGroupBlock(uint8_t record_id_size_bytes, uint64_t record_id);
    virtual ~ChannelGroupBlock();

    uint16_t GetFlags() const;
    uint64_t GetRecordId() const;
    uint8_t GetRecordIdSizeBytes() const;
    /** The record ID as it prefixes a record: the first GetRecordIdSizeBytes() bytes, the rest zero. */
    std::array<uint8_t, 8> GetRecordIdData() const;
    uint32_t GetDataSizeBytes() const;
    uint32_t GetInvalidationBytes() const;
    uint64_t GetCycleCount() const;
    uint64_t GetVlsdDataSizeBytes() const;
    void IncrementCycleCount();
    void AddVlsdDataBytes(uint64_t bytes);
    void ResetCounters();
    std::vector<std::shared_ptr<ChannelBlock>> GetChannels() const;

    uint64_t GetBlockSize() const override;
    std::unique_ptr<uint8_t[]> Serialize() const override;
    const IBlockLinks* GetBlockLinks() const override { return &links_; }
    std::shared_ptr<ISerializableBlock> GetNext() const override {
        return links_.GetLink(LinkType::ChannelGroupNext);
    }
    std::vector<std::shared_ptr<ISerializableBlock>> GetChildren() const override {
        return {
            links_.GetLink(LinkType::MetadataComment),
            links_.GetLink(LinkType::TextAcquisitionName),
            links_.GetLink(LinkType::SourceInformation),
            links_.GetLink(LinkType::SampleReductionFirst),
            links_.GetLink(LinkType::ChannelFirst) };
    }

    void AddChannel(std::shared_ptr<ChannelBlock> channel);
    /** Appends @p next_block to the end of the chain this block belongs to. */
    void LinkBlock(std::shared_ptr<ChannelGroupBlock> next_block);
    void UnlinkNext();
    void AddSourceInformation(std::shared_ptr<SourceInformationBlock> source_information);
    /** Throws for VlsdChannel on a group with invalidation bytes: see SetInvalidationBytes(). */
    void SetFlags(uint16_t flags);
    /**
     * Bytes after the data bytes of every record that hold the channels' invalidation bits. Throws
     * on a VLSD group, whose cg_inval_bytes field holds the high half of its total value size.
     */
    void SetInvalidationBytes(uint32_t invalidation_bytes);
    void SetPathSeparator(uint16_t path_separator);
    void SetName(std::shared_ptr<TextBlock> name);
    void SetComment(std::shared_ptr<MetadataBlock> comment);
};

} // namespace eerie_leap::subsys::mdf::mdf4
