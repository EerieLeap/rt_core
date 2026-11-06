#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "block_base.h"
#include "metadata_block.h"
#include "subsys/mdf/utilities/block_links.h"
#include "text_block.h"

namespace eerie_leap::subsys::mdf::mdf4 {

/** ##EV (MDF 4.1): one event with no scope and no attachment. */
class EventBlock : public BlockBase {
public:
    enum class Type : uint8_t {
        Recording = 0,
        RecordingInterrupt,
        AcquisitionInterrupt,
        StartRecordingTrigger,
        StopRecordingTrigger,
        Trigger,
        Marker
    };

    enum class SyncType : uint8_t {
        Time = 1,
        Angle,
        Distance,
        Index
    };

    enum class RangeType : uint8_t {
        Point = 0,
        RangeBegin,
        RangeEnd
    };

    enum class Cause : uint8_t {
        Other = 0,
        Error,
        Tool,
        Script,
        User
    };

private:
    enum class LinkType : int {
        EventNext = 0,
        EventParent,
        EventRange,
        TxName,
        MdComment
    };

    utilities::BlockLinks<LinkType, 5> links_;

    Type type_{ Type::Recording };             // 1 byte, ev_type
    SyncType sync_type_{ SyncType::Time };     // 1 byte, ev_sync_type
    RangeType range_type_{ RangeType::Point }; // 1 byte, ev_range_type
    Cause cause_{ Cause::Other };              // 1 byte, ev_cause
    uint8_t flags_{ 0 };                       // 1 byte, ev_flags
    // uint8_t reserved_1_[3];                  // 3 bytes, Reserved
    uint32_t scope_count_{ 0 };      // 4 bytes, ev_scope_count
    uint16_t attachment_count_{ 0 }; // 2 bytes, ev_attachment_count
    uint16_t creator_index_{ 0 };    // 2 bytes, ev_creator_index
    int64_t sync_base_value_{ 0 };   // 8 bytes, ev_sync_base_value
    double sync_factor_{ 1.0 };      // 8 bytes, ev_sync_factor

public:
    EventBlock();
    ~EventBlock() override = default;

    void SetType(Type type) {
        type_ = type;
    }
    void SetSyncType(SyncType sync_type) {
        sync_type_ = sync_type;
    }
    void SetRangeType(RangeType range_type) {
        range_type_ = range_type;
    }
    void SetCause(Cause cause) {
        cause_ = cause;
    }
    void SetSyncBaseValue(int64_t value) {
        sync_base_value_ = value;
    }
    void SetSyncFactor(double factor) {
        sync_factor_ = factor;
    }
    void SetName(std::shared_ptr<TextBlock> name);
    void SetComment(std::shared_ptr<MetadataBlock> comment);

    uint64_t GetBlockSize() const override;
    std::unique_ptr<uint8_t[]> Serialize() const override;
    const IBlockLinks* GetBlockLinks() const override {
        return &links_;
    }
    std::shared_ptr<ISerializableBlock> GetNext() const override {
        return nullptr;
    }
    std::vector<std::shared_ptr<ISerializableBlock>> GetChildren() const override {
        return { links_.GetLink(LinkType::TxName), links_.GetLink(LinkType::MdComment) };
    }
};

} // namespace eerie_leap::subsys::mdf::mdf4
