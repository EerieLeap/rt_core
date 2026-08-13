#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <vector>
#include <fstream>

#include <zephyr/ztest.h>

#include "subsys/time/rtc_provider.h"
#include "subsys/mdf/mdf4/channel_group_block.h"
#include "subsys/mdf/mdf4/source_information_block.h"
#include "subsys/mdf/mdf_data_type.h"
#include "subsys/mdf/mdf_value.h"
#include "subsys/mdf/mdf4_file.h"

using namespace eerie_leap::subsys::time;
using namespace eerie_leap::subsys::mdf;
using eerie_leap::subsys::canbus::CanFrame;

namespace {

constexpr size_t ID_BLOCK_SIZE = 64;
constexpr size_t BLOCK_HEADER_LENGTH_OFFSET = 8;
constexpr uint8_t RECORD_ID_SIZE = 1;

uint64_t ReadUint64(const std::string& data, size_t offset) {
    uint64_t value = 0;
    zassert_true(offset + sizeof(value) <= data.size(), "Read past end of file");
    std::memcpy(&value, data.data() + offset, sizeof(value));

    return value;
}

uint32_t ReadUint32(const std::string& data, size_t offset) {
    uint32_t value = 0;
    zassert_true(offset + sizeof(value) <= data.size(), "Read past end of file");
    std::memcpy(&value, data.data() + offset, sizeof(value));

    return value;
}

// Walks the block chain from the end of the ID block and returns the address of the DT block.
size_t FindDataBlockAddress(const std::string& data) {
    size_t offset = ID_BLOCK_SIZE;
    while(offset + 24 <= data.size()) {
        std::string id(data.data() + offset, 4);
        auto length = ReadUint64(data, offset + BLOCK_HEADER_LENGTH_OFFSET);
        zassert_true(length >= 24, "Block %s has an invalid length", id.c_str());

        if(id == "##DT")
            return offset;

        offset += length;
    }

    return 0;
}

} // namespace

ZTEST_SUITE(mdf_file, NULL, NULL, NULL, NULL, NULL);

ZTEST(mdf_file, test_WriteAndFinalize) {
    RtcProvider rtc_time_provider;

    Mdf4File mdf_file(RECORD_ID_SIZE);
    mdf_file.UpdateCurrentTime(rtc_time_provider.GetTime());

    auto channel_group_1 = mdf_file.CreateChannelGroup(1, "pressure");
    auto source_information = std::make_shared<mdf4::SourceInformationBlock>(
        mdf4::SourceInformationBlock::SourceType::IoDevice,
        mdf4::SourceInformationBlock::BusType::None);
    source_information->SetName(mdf_file.GetOrCreateTextBlock("Eerie Leap Sensor"));
    channel_group_1->AddSourceInformation(source_information);

    auto channel_1 = mdf_file.CreateDataChannel(channel_group_1, MdfDataType::Uint64, "value", "bar");
    channel_1->SetConversion(mdf_file.CreateAlgebraicConversion("x * 0.1"));

    auto channel_group_2 = mdf_file.CreateChannelGroup(2, "temperature");
    channel_group_2->AddSourceInformation(source_information);
    mdf_file.CreateDataChannel(channel_group_2, MdfDataType::Float32, "value", "C");

    auto vlsd_channel_group_3 = mdf_file.CreateVLSDChannelGroup(3);
    auto channel_group_3 = mdf_file.CreateCanDataFrameChannelGroup(vlsd_channel_group_3, 4, "Raw CAN Frame");

    std::stringbuf file(std::ios::in | std::ios::out | std::ios::binary);
    // Will create file in the twister-out directory
    // std::ofstream file_stream("../../../../../../../../test_file.mf4", std::ios::binary);
    // auto& file = *file_stream.rdbuf();

    auto header_bytes_written = mdf_file.WriteHeaderToStream(file);
    zassert_true(header_bytes_written > ID_BLOCK_SIZE);

    constexpr uint64_t value_record_count = 10;
    for(uint64_t i = 0; i < value_record_count; i++) {
        std::array<MdfValue, 2> values{static_cast<float>(i) * 0.1F, i * i};
        mdf_file.WriteDataRecordToStream(channel_group_1, file, values);
    }

    for(uint64_t i = 0; i < value_record_count; i++) {
        std::array<MdfValue, 2> values{static_cast<float>(i) * 0.1F, static_cast<float>(i)};
        mdf_file.WriteDataRecordToStream(channel_group_2, file, values);
    }

    constexpr uint64_t can_record_count = 10;
    constexpr size_t can_payload_size = 8;
    for(uint64_t i = 0; i < can_record_count; i++) {
        CanFrame can_frame {
            .id = 0x18FF1234,
            .is_extended = true,
            .is_transmit = false,
            .is_can_fd = false,
            .is_bitrate_switch = false,
            .data = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0}
        };

        mdf_file.WriteCanbusDataRecordToStream(channel_group_3, file, can_frame, static_cast<float>(i) * 0.1F);
    }

    zassert_equal(channel_group_1->GetCycleCount(), value_record_count);
    zassert_equal(channel_group_2->GetCycleCount(), value_record_count);
    zassert_equal(channel_group_3->GetCycleCount(), can_record_count);
    zassert_equal(vlsd_channel_group_3->GetCycleCount(), can_record_count);
    zassert_equal(vlsd_channel_group_3->GetVlsdDataSizeBytes(), can_record_count * can_payload_size);

    mdf_file.FinalizeToStream(file);

    // file_stream.close();

    const auto data = file.str();

    // File identifier is switched to finalized and the unfinalized flags are cleared.
    zassert_mem_equal(data.data(), "MDF     ", 8);
    zassert_mem_equal(data.data() + 8, "4.10    ", 8);
    zassert_equal(ReadUint32(data, 60), 0, "Unfinalized flags were not cleared");

    // The DT block length has to cover every record appended after the header.
    const auto data_block_address = FindDataBlockAddress(data);
    zassert_not_equal(data_block_address, 0, "DT block not found");
    zassert_equal(data_block_address + 24, header_bytes_written, "DT block is not the last header block");

    const uint64_t expected_record_bytes =
        value_record_count * (RECORD_ID_SIZE + channel_group_1->GetDataSizeBytes())
        + value_record_count * (RECORD_ID_SIZE + channel_group_2->GetDataSizeBytes())
        + can_record_count * (RECORD_ID_SIZE + channel_group_3->GetDataSizeBytes())
        + can_record_count * (RECORD_ID_SIZE + 4 + can_payload_size);

    zassert_equal(ReadUint64(data, data_block_address + BLOCK_HEADER_LENGTH_OFFSET), 24 + expected_record_bytes);
    zassert_equal(data.size(), header_bytes_written + expected_record_bytes);
}

ZTEST(mdf_file, test_CanFdFrameEncoding) {
    Mdf4File mdf_file(1);

    auto vlsd_channel_group = mdf_file.CreateVLSDChannelGroup(1);
    auto channel_group = mdf_file.CreateCanDataFrameChannelGroup(vlsd_channel_group, 2, "Raw CAN Frame");

    std::stringbuf file(std::ios::in | std::ios::out | std::ios::binary);
    auto header_bytes_written = mdf_file.WriteHeaderToStream(file);

    CanFrame can_frame {
        .id = 0x7FF,
        .is_extended = false,
        .is_transmit = true,
        .is_can_fd = true,
        .is_bitrate_switch = true,
        .data = std::vector<uint8_t>(12, 0xAA)
    };

    mdf_file.WriteCanbusDataRecordToStream(channel_group, file, can_frame, 1.5F);
    mdf_file.FinalizeToStream(file);

    const auto data = file.str();
    const size_t record = header_bytes_written + 1; // skip the record ID

    float timestamp = 0;
    std::memcpy(&timestamp, data.data() + record, sizeof(timestamp));
    zassert_equal(timestamp, 1.5F);

    const auto id_pack = ReadUint32(data, record + 4);
    zassert_equal(id_pack & 0x3U, 0, "bus channel");
    zassert_equal((id_pack >> 2) & 0x1FFFFFFFU, 0x7FFU, "frame id");
    zassert_equal((id_pack >> 31) & 0x1U, 0, "IDE must be clear for a standard id");

    const auto dir_length = static_cast<uint8_t>(data[record + 8]);
    zassert_equal(dir_length & 0x1U, 1, "direction");
    zassert_equal((dir_length >> 1) & 0x7FU, 12, "data length");

    const auto edl_brs_dlc = static_cast<uint8_t>(data[record + 9]);
    zassert_equal(edl_brs_dlc & 0x1U, 1, "EDL");
    zassert_equal((edl_brs_dlc >> 1) & 0x1U, 1, "BRS");
    zassert_equal((edl_brs_dlc >> 2) & 0xFU, 9, "DLC code for a 12 byte CAN FD payload");

    zassert_equal(ReadUint32(data, record + 10), 0, "VLSD offset of the first frame");
    zassert_equal(vlsd_channel_group->GetVlsdDataSizeBytes(), 12);
}

ZTEST(mdf_file, test_UnfinalizedFileIsMarkedAsSuch) {
    Mdf4File mdf_file(1);

    auto channel_group = mdf_file.CreateChannelGroup(1, "pressure");
    mdf_file.CreateDataChannel(channel_group, MdfDataType::Float32, "value", "bar");

    std::stringbuf file(std::ios::in | std::ios::out | std::ios::binary);
    mdf_file.WriteHeaderToStream(file);

    for(uint64_t i = 0; i < 4; i++) {
        std::array<MdfValue, 2> values{static_cast<float>(i), static_cast<float>(i)};
        mdf_file.WriteDataRecordToStream(channel_group, file, values);
    }

    // Never finalized, as would happen after a power loss.
    const auto data = file.str();
    zassert_mem_equal(data.data(), "UnFinMF ", 8);

    constexpr uint32_t expected_flags = 0x01 | 0x04 | 0x20;
    zassert_equal(ReadUint32(data, 60) & 0xFFFFU, expected_flags, "unfinalized flags must be set");
}

ZTEST(mdf_file, test_RejectsRecordIdThatDoesNotFit) {
    Mdf4File mdf_file(1);

    mdf_file.CreateChannelGroup(255, "ok");

    bool threw = false;
    try {
        mdf_file.CreateChannelGroup(256, "too_large");
    } catch(const std::runtime_error&) {
        threw = true;
    }
    zassert_true(threw, "a record ID wider than the record ID size should be rejected");

    threw = false;
    try {
        mdf_file.CreateChannelGroup(255, "duplicate");
    } catch(const std::runtime_error&) {
        threw = true;
    }
    zassert_true(threw, "a duplicate record ID should be rejected");
}

ZTEST(mdf_file, test_RejectsMismatchedValueTypes) {
    Mdf4File mdf_file(1);

    auto channel_group = mdf_file.CreateChannelGroup(1, "pressure");
    mdf_file.CreateDataChannel(channel_group, MdfDataType::Uint64, "value", "bar");

    std::stringbuf file(std::ios::in | std::ios::out | std::ios::binary);
    mdf_file.WriteHeaderToStream(file);

    bool threw = false;
    try {
        std::array<MdfValue, 2> wrong_type{0.0F, 1.0F};
        mdf_file.WriteDataRecordToStream(channel_group, file, wrong_type);
    } catch(const std::runtime_error&) {
        threw = true;
    }
    zassert_true(threw, "a value type that does not match the channel should be rejected");

    threw = false;
    try {
        std::array<MdfValue, 1> wrong_count{0.0F};
        mdf_file.WriteDataRecordToStream(channel_group, file, wrong_count);
    } catch(const std::runtime_error&) {
        threw = true;
    }
    zassert_true(threw, "a wrong number of values should be rejected");
}
