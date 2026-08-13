#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <streambuf>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <chrono>

#include "subsys/canbus/can_frame.h"
#include "subsys/mdf/mdf4/channel_conversion_block.h"
#include "subsys/mdf/mdf4/channel_group_block.h"
#include "subsys/mdf/mdf4/data_group_block.h"
#include "subsys/mdf/mdf4/id_block.h"
#include "subsys/mdf/mdf4/header_block.h"
#include "subsys/mdf/mdf4/data_record.h"
#include "subsys/mdf/mdf4/vlsd_data_record.h"

#include "mdf_data_type.h"
#include "mdf_value.h"

namespace eerie_leap::subsys::mdf {

using eerie_leap::subsys::canbus::CanFrame;

/**
 * Writer for a single measurement MDF 4.10 file.
 *
 * Usage is strictly ordered: describe the channel groups, then WriteHeaderToStream(),
 * then append records, then FinalizeToStream(). The header is written with the
 * "unfinalized" identifier so a file left behind by a power loss stays recoverable;
 * FinalizeToStream() seeks back and patches the DT block length, the channel group
 * cycle counts and the file identifier, which requires a seekable output stream.
 */
class Mdf4File {
private:
    struct CanDataFrameBlocks {
        std::shared_ptr<mdf4::DataRecord> header_data_record;
        std::shared_ptr<mdf4::VlsdDataRecord> raw_data_vlsd_data_record;
    };

    std::unique_ptr<mdf4::IdBlock> id_block_;
    std::unique_ptr<mdf4::HeaderBlock> header_block_;
    std::shared_ptr<mdf4::DataGroupBlock> data_group_;

    std::unordered_set<uint64_t> record_ids_;
    std::vector<std::shared_ptr<mdf4::ChannelGroupBlock>> channel_groups_;
    std::unordered_map<std::string, std::shared_ptr<mdf4::TextBlock>> text_blocks_;
    std::unordered_map<std::shared_ptr<mdf4::ChannelGroupBlock>, std::shared_ptr<mdf4::DataRecord>> data_records_;
    std::unordered_map<std::shared_ptr<mdf4::ChannelGroupBlock>, CanDataFrameBlocks> can_data_frame_blocks_;

    uint64_t data_bytes_written_;
    bool is_header_written_;
    bool is_finalized_;

    std::shared_ptr<mdf4::ChannelBlock> CreateChannelBlock(MdfDataType data_type, const std::string& name, const std::string& unit = "");
    std::shared_ptr<mdf4::ChannelGroupBlock> CreateChannelGroupBlock(uint64_t record_id);
    void EnsureRecordsWritable() const;

public:
    static constexpr const char* LOG_DATA_FILE_EXTENSION = "mf4";

    explicit Mdf4File(uint8_t record_id_size_bytes = 4);
    ~Mdf4File() = default;

    Mdf4File(const Mdf4File&) = delete;
    Mdf4File& operator=(const Mdf4File&) = delete;

    void UpdateCurrentTime(std::chrono::system_clock::time_point time);

    std::shared_ptr<mdf4::DataGroupBlock> GetDataGroup() const;

    std::shared_ptr<mdf4::ChannelGroupBlock> CreateChannelGroup(uint64_t record_id, const std::string& name);
    std::shared_ptr<mdf4::ChannelGroupBlock> CreateVLSDChannelGroup(uint64_t record_id);
    std::shared_ptr<mdf4::ChannelGroupBlock> CreateCanDataFrameChannelGroup(
        const std::shared_ptr<mdf4::ChannelGroupBlock>& vlsd_channel_group,
        uint64_t record_id,
        const std::string& name);
    std::shared_ptr<mdf4::ChannelBlock> CreateDataChannel(
        const std::shared_ptr<mdf4::ChannelGroupBlock>& channel_group,
        MdfDataType data_type,
        const std::string& name,
        const std::string& unit);
    std::shared_ptr<mdf4::ChannelConversionBlock> CreateAlgebraicConversion(const std::string& formula);
    std::shared_ptr<mdf4::TextBlock> GetOrCreateTextBlock(const std::string& name);

    uint64_t WriteHeaderToStream(std::streambuf& stream);
    uint64_t WriteDataRecordToStream(
        const std::shared_ptr<mdf4::ChannelGroupBlock>& channel_group,
        std::streambuf& stream,
        std::span<const MdfValue> values);
    uint64_t WriteCanbusDataRecordToStream(
        const std::shared_ptr<mdf4::ChannelGroupBlock>& channel_group,
        std::streambuf& stream,
        const CanFrame& can_frame,
        float time);
    /** Patches the DT length, cycle counts and file identifier. Requires a seekable stream. */
    uint64_t FinalizeToStream(std::streambuf& stream);
};

} // namespace eerie_leap::subsys::mdf
