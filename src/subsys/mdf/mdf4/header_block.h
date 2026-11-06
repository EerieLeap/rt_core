#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "attachment_block.h"
#include "block_base.h"
#include "data_group_block.h"
#include "event_block.h"
#include "file_history_block.h"
#include "metadata_block.h"
#include "subsys/mdf/utilities/block_links.h"

namespace eerie_leap::subsys::mdf::mdf4 {

class HeaderBlock : public BlockBase {
private:
    enum class LinkType : int {
        DataGroupFirst = 0,
        FileHistoryFirst,
        ChannelHierarchyFirst,
        AttachmentFirst,
        EventFirst,
        MetadataComment
    };

    utilities::BlockLinks<LinkType, 6> links_;

    uint64_t start_time_ns_{ 0 }; // 8 bytes, Start time in ns
    int16_t tz_offset_min_{ 0 };  // 2 bytes, Timezone offset in minutes
    int16_t dst_offset_min_{ 0 }; // 2 bytes, Daylight saving time (DST) offset in minutes
    uint8_t time_flags_{ 0 };     // 1 bytes, Time flags
    uint8_t time_class_{ 0 };     // 1 bytes, Time quality class
    uint8_t flags_{ 0 };          // 1 bytes, Flags
    // std::vector<uint8_t> reserved_1_;    // 1 bytes, Reserved
    double hd_start_angle_rad_{ 0.0 }; // 8 bytes, Start angle in radians at start of measurement
    double hd_distance_m_{ 0.0 };      // 8 bytes, Start distance in meters at start of measurement

public:
    HeaderBlock();
    ~HeaderBlock() override = default;

    void SetCurrentTimeNs(uint64_t time_ns);
    void SetTimeClass(uint8_t time_class) {
        time_class_ = time_class;
    }
    void SetComment(std::shared_ptr<MetadataBlock> comment);
    std::shared_ptr<FileHistoryBlock> GetFileHistory() const;
    /** Links the first event of the file; null unlinks. */
    void SetEventFirst(std::shared_ptr<EventBlock> event);
    bool HasEvent() const {
        return links_.GetLink(LinkType::EventFirst) != nullptr;
    }

    uint64_t GetBlockSize() const override;
    std::unique_ptr<uint8_t[]> Serialize() const override;
    const IBlockLinks* GetBlockLinks() const override {
        return &links_;
    }
    std::shared_ptr<ISerializableBlock> GetNext() const override {
        return nullptr;
    }
    std::vector<std::shared_ptr<ISerializableBlock>> GetChildren() const override {
        return { links_.GetLink(LinkType::MetadataComment), links_.GetLink(LinkType::FileHistoryFirst),
            links_.GetLink(LinkType::ChannelHierarchyFirst), links_.GetLink(LinkType::AttachmentFirst),
            links_.GetLink(LinkType::EventFirst), links_.GetLink(LinkType::DataGroupFirst) };
    }

    void AddFileHistory(std::shared_ptr<FileHistoryBlock> file_history);
    void AddAttachment(std::shared_ptr<AttachmentBlock> attachment);
    void AddDataGroup(std::shared_ptr<DataGroupBlock> data_group);
};

} // namespace eerie_leap::subsys::mdf::mdf4
