#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <streambuf>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "subsys/canbus/can_frame.h"
#include "subsys/mdf/mdf4/can_record_writer.h"
#include "subsys/mdf/mdf4/channel_conversion_block.h"
#include "subsys/mdf/mdf4/channel_group_block.h"
#include "subsys/mdf/mdf4/data_group_block.h"
#include "subsys/mdf/mdf4/header_block.h"
#include "subsys/mdf/mdf4/id_block.h"
#include "subsys/mdf/mdf4/record_writer.h"

#include "mdf_data_type.h"
#include "mdf_property.h"
#include "mdf_value.h"

namespace eerie_leap::subsys::mdf {

using eerie_leap::subsys::canbus::CanFrame;

struct Mdf4FileOptions {
    uint8_t record_id_size_bytes = 1;
    MdfDataType time_type = MdfDataType::Float64; // master channel of every group: seconds since the header start time
    uint8_t vlsd_offset_size_bytes = 8;           // width of the in-record VLSD offset (CAN_DataFrame.DataBytes)
};

/**
 * Writer for a single measurement MDF 4.10 file.
 *
 * Usage is strictly ordered: describe the channel groups, then WriteHeaderToStream(),
 * then append records, then FinalizeToStream(). The header is written with the
 * "unfinalized" identifier so a file left behind by a power loss stays recoverable;
 * FinalizeToStream() seeks back and patches the DT block length, the channel group
 * cycle counts and the file identifier, which requires a seekable output stream.
 *
 * Records go through the writers WriteHeaderToStream() builds from the sealed layout: one per
 * channel group created with CreateChannelGroup() and one per CAN data frame group.
 */
class Mdf4File {
private:
    struct CanChannelGroups {
        std::shared_ptr<mdf4::ChannelGroupBlock> channel_group;
        std::shared_ptr<mdf4::ChannelGroupBlock> vlsd_channel_group;
    };

    Mdf4FileOptions options_;

    std::unique_ptr<mdf4::IdBlock> id_block_;
    std::unique_ptr<mdf4::HeaderBlock> header_block_;
    std::shared_ptr<mdf4::DataGroupBlock> data_group_;

    std::unordered_set<uint64_t> record_ids_;
    std::vector<std::shared_ptr<mdf4::ChannelGroupBlock>> channel_groups_;
    std::vector<std::shared_ptr<mdf4::ChannelGroupBlock>> record_channel_groups_;
    std::vector<CanChannelGroups> can_channel_groups_;
    std::unordered_map<std::string, std::shared_ptr<mdf4::TextBlock>> text_blocks_;

    // Rebuilt by every WriteHeaderToStream(); callers keep references until the next one.
    std::vector<mdf4::RecordWriter> record_writers_;
    std::vector<mdf4::CanRecordWriter> can_record_writers_;

    bool is_header_written_;
    bool is_finalized_;

    std::shared_ptr<mdf4::ChannelBlock>
    CreateChannelBlock(MdfDataType data_type, const std::string& name, const std::string& unit = "");
    std::shared_ptr<mdf4::ChannelGroupBlock> CreateChannelGroupBlock(uint64_t record_id);
    std::shared_ptr<mdf4::ChannelGroupBlock> CreateTimeChannelGroup(uint64_t record_id, const std::string& name);
    void BuildRecordWriters();
    std::shared_ptr<mdf4::MetadataBlock> CreateCommentBlock(
        const std::string& block_id,
        std::string_view text,
        std::span<const MdfProperty> properties = {}
    );
    void EnsureRecordsWritable() const;
    void EnsureStructureEditable() const;

public:
    static constexpr const char* LOG_DATA_FILE_EXTENSION = "mf4";

    explicit Mdf4File(Mdf4FileOptions options = {});
    ~Mdf4File();

    Mdf4File(const Mdf4File&) = delete;
    Mdf4File& operator=(const Mdf4File&) = delete;

    void UpdateCurrentTime(std::chrono::system_clock::time_point time);

    std::shared_ptr<mdf4::DataGroupBlock> GetDataGroup() const;

    /** The metadata setters, like the structure, must be called before WriteHeaderToStream(). */
    void SetHeaderComment(std::string_view text, std::span<const MdfProperty> properties);
    void SetFileHistory(std::string_view tool_id, std::string_view tool_vendor, std::string_view tool_version);
    void SetChannelComment(const std::shared_ptr<mdf4::ChannelBlock>& channel, std::string_view text);
    void SetChannelGroupComment(const std::shared_ptr<mdf4::ChannelGroupBlock>& channel_group, std::string_view text);

    std::shared_ptr<mdf4::ChannelGroupBlock> CreateChannelGroup(uint64_t record_id, const std::string& name);
    std::shared_ptr<mdf4::ChannelGroupBlock> CreateVLSDChannelGroup(uint64_t record_id);
    std::shared_ptr<mdf4::ChannelGroupBlock> CreateCanDataFrameChannelGroup(
        const std::shared_ptr<mdf4::ChannelGroupBlock>& vlsd_channel_group,
        uint64_t record_id,
        const std::string& name
    );
    /**
     * Rejects a channel that would take the group's record past RecordWriter's limits. With
     * @p with_invalidation_bit the channel gets the next bit of the group's one invalidation byte,
     * which follows the data bytes of every record: up to eight channels of a group can have one.
     */
    std::shared_ptr<mdf4::ChannelBlock> CreateDataChannel(
        const std::shared_ptr<mdf4::ChannelGroupBlock>& channel_group,
        MdfDataType data_type,
        const std::string& name,
        const std::string& unit,
        bool with_invalidation_bit = false
    );
    std::shared_ptr<mdf4::ChannelConversionBlock> CreateAlgebraicConversion(const std::string& formula);
    std::shared_ptr<mdf4::TextBlock> GetOrCreateTextBlock(const std::string& name);

    /** Seals the layout: builds the record writers and resets every counter for a new file. */
    uint64_t WriteHeaderToStream(std::streambuf& stream);

    /**
     * The writer of a group from CreateChannelGroup() or CreateCanDataFrameChannelGroup(), valid from
     * WriteHeaderToStream() until the next one or the file's destruction. Throws before the header
     * is written and for a group of another kind or another file. Do not write through a writer after
     * FinalizeToStream(): its records would land past the finalized DT length.
     */
    mdf4::RecordWriter& GetRecordWriter(const std::shared_ptr<mdf4::ChannelGroupBlock>& channel_group);
    mdf4::CanRecordWriter& GetCanRecordWriter(const std::shared_ptr<mdf4::ChannelGroupBlock>& channel_group);

    /** Convenience wrappers that look the writer up on every call; a hot path keeps the writer. */
    uint64_t WriteDataRecordToStream(
        const std::shared_ptr<mdf4::ChannelGroupBlock>& channel_group,
        std::streambuf& stream,
        std::span<const MdfValue> values,
        uint32_t invalid_mask = 0
    );
    uint64_t WriteCanbusDataRecordToStream(
        const std::shared_ptr<mdf4::ChannelGroupBlock>& channel_group,
        std::streambuf& stream,
        const CanFrame& can_frame,
        double time,
        uint8_t bus_channel
    );
    /**
     * Patches the DT length and the cycle counts, syncs the stream, then patches the file
     * identifier and syncs again, so the identifier never reaches the medium ahead of the lengths
     * it vouches for. Requires a seekable stream; a failed seek or sync throws std::ios_base::failure
     * and leaves the file unfinalized.
     */
    uint64_t FinalizeToStream(std::streambuf& stream);
};

} // namespace eerie_leap::subsys::mdf
