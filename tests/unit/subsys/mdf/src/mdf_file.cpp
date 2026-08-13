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

#include "mdf_test_support.h"

using namespace eerie_leap::subsys::time;
using namespace eerie_leap::subsys::mdf;
using namespace mdf_test;
using eerie_leap::subsys::canbus::CanFrame;

namespace {

constexpr uint8_t RECORD_ID_SIZE = 1;
constexpr size_t ID_STANDARD_FLAGS_OFFSET = 60;

// Walks the block chain from the end of the ID block and returns the address of the DT block.
size_t FindDataBlockAddress(std::span<const uint8_t> data) {
    size_t offset = ID_BLOCK_SIZE;
    while(offset + BLOCK_HEADER_SIZE <= data.size()) {
        const auto id = BlockId(data, offset);
        const auto length = Read<uint64_t>(data, offset + BLOCK_LENGTH_OFFSET);
        zassert_true(length >= BLOCK_HEADER_SIZE, "Block %s has an invalid length", id.c_str());

        if(id == "##DT")
            return offset;

        offset += length;
    }

    return 0;
}

CanFrame MakeCanFrame(size_t payload_size) {
    return CanFrame {
        .id = 0x100,
        .is_extended = false,
        .is_transmit = false,
        .is_can_fd = payload_size > 8,
        .is_bitrate_switch = false,
        .data = std::vector<uint8_t>(payload_size, 0x5A)
    };
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

    const auto file_bytes = file.str();
    const auto data = AsBytes(file_bytes);

    // File identifier is switched to finalized and the unfinalized flags are cleared.
    zassert_mem_equal(data.data(), "MDF     ", 8);
    zassert_mem_equal(data.data() + 8, "4.10    ", 8);
    zassert_equal(Read<uint32_t>(data, ID_STANDARD_FLAGS_OFFSET), 0, "Unfinalized flags were not cleared");

    // The DT block length has to cover every record appended after the header.
    const auto data_block_address = FindDataBlockAddress(data);
    zassert_not_equal(data_block_address, 0, "DT block not found");
    zassert_equal(data_block_address + BLOCK_HEADER_SIZE, header_bytes_written, "DT block is not the last header block");

    const uint64_t expected_record_bytes =
        value_record_count * (RECORD_ID_SIZE + channel_group_1->GetDataSizeBytes())
        + value_record_count * (RECORD_ID_SIZE + channel_group_2->GetDataSizeBytes())
        + can_record_count * (RECORD_ID_SIZE + channel_group_3->GetDataSizeBytes())
        + can_record_count * (RECORD_ID_SIZE + 4 + can_payload_size);

    zassert_equal(Read<uint64_t>(data, data_block_address + BLOCK_LENGTH_OFFSET),
        BLOCK_HEADER_SIZE + expected_record_bytes);
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

    const auto file_bytes = file.str();
    const auto data = AsBytes(file_bytes);
    const size_t record = header_bytes_written + 1; // skip the record ID

    float timestamp = 0;
    std::memcpy(&timestamp, data.data() + record, sizeof(timestamp));
    zassert_equal(timestamp, 1.5F);

    const auto id_pack = Read<uint32_t>(data, record + 4);
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

    zassert_equal(Read<uint32_t>(data, record + 10), 0, "VLSD offset of the first frame");
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
    const auto file_bytes = file.str();
    const auto data = AsBytes(file_bytes);
    zassert_mem_equal(data.data(), "UnFinMF ", 8);

    constexpr uint32_t expected_flags = 0x01 | 0x04 | 0x20;
    zassert_equal(Read<uint32_t>(data, ID_STANDARD_FLAGS_OFFSET) & 0xFFFFU, expected_flags,
        "unfinalized flags must be set");
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

ZTEST(mdf_file, test_RejectsZeroRecordIdSize) {
    bool threw = false;
    try {
        Mdf4File mdf_file(0);
    } catch(const std::invalid_argument&) {
        threw = true;
    }
    zassert_true(threw, "records of several channel groups share one data block and need an id");
}

ZTEST(mdf_file, test_StructureIsFrozenOnceTheHeaderIsWritten) {
    Mdf4File mdf_file(1);
    auto channel_group = mdf_file.CreateChannelGroup(1, "pressure");

    auto file = MakeStream();
    mdf_file.WriteHeaderToStream(file);

    // Adding blocks now would invalidate every address already written to the stream.
    zassert_true(Throws<std::runtime_error>([&] { mdf_file.CreateChannelGroup(2, "late"); }));
    zassert_true(Throws<std::runtime_error>([&] { mdf_file.CreateVLSDChannelGroup(3); }));
    zassert_true(Throws<std::runtime_error>([&] {
        mdf_file.CreateDataChannel(channel_group, MdfDataType::Float32, "late", "");
    }));
}

ZTEST(mdf_file, test_RejectsRecordsOutsideTheWritableWindow) {
    Mdf4File mdf_file(1);
    auto channel_group = mdf_file.CreateChannelGroup(1, "pressure");

    auto file = MakeStream();
    std::array<MdfValue, 1> values{0.0F};

    zassert_true(Throws<std::runtime_error>([&] { mdf_file.WriteDataRecordToStream(channel_group, file, values); }),
        "records written before the header would land in front of the DT block");

    mdf_file.WriteHeaderToStream(file);
    mdf_file.WriteDataRecordToStream(channel_group, file, values);
    mdf_file.FinalizeToStream(file);

    zassert_true(Throws<std::runtime_error>([&] { mdf_file.WriteDataRecordToStream(channel_group, file, values); }),
        "records written after finalizing would not be covered by the patched DT length");
}

ZTEST(mdf_file, test_RejectsChannelGroupsFromAnotherFile) {
    Mdf4File mdf_file(1);
    mdf_file.CreateChannelGroup(1, "pressure");

    Mdf4File other_file(1);
    auto foreign_group = other_file.CreateChannelGroup(1, "pressure");

    auto file = MakeStream();
    mdf_file.WriteHeaderToStream(file);

    std::array<MdfValue, 1> values{0.0F};
    zassert_true(Throws<std::runtime_error>([&] { mdf_file.WriteDataRecordToStream(foreign_group, file, values); }));

    auto can_frame = MakeCanFrame(8);
    zassert_true(Throws<std::runtime_error>([&] {
        mdf_file.WriteCanbusDataRecordToStream(foreign_group, file, can_frame, 0.0F);
    }));
}

ZTEST(mdf_file, test_CanDataFrameGroupRequiresAVlsdGroup) {
    Mdf4File mdf_file(1);

    zassert_true(Throws<std::invalid_argument>([&] {
        mdf_file.CreateCanDataFrameChannelGroup(nullptr, 1, "Raw CAN Frame");
    }), "a null VLSD group would leave the payload channel dangling");

    auto plain_group = mdf_file.CreateChannelGroup(1, "plain");
    zassert_true(Throws<std::runtime_error>([&] {
        mdf_file.CreateCanDataFrameChannelGroup(plain_group, 2, "Raw CAN Frame");
    }), "a group without the VLSD flag cannot hold variable length payloads");
}

ZTEST(mdf_file, test_CanDataFrameGroupRejectsExtraChannels) {
    Mdf4File mdf_file(1);

    auto vlsd_channel_group = mdf_file.CreateVLSDChannelGroup(1);
    auto can_group = mdf_file.CreateCanDataFrameChannelGroup(vlsd_channel_group, 2, "Raw CAN Frame");

    // The CAN record layout is fixed, so appending a channel would desynchronize the writer.
    zassert_true(Throws<std::runtime_error>([&] {
        mdf_file.CreateDataChannel(can_group, MdfDataType::Float32, "extra", "");
    }));
    zassert_true(Throws<std::runtime_error>([&] {
        mdf_file.CreateDataChannel(vlsd_channel_group, MdfDataType::Float32, "extra", "");
    }));

    auto file = MakeStream();
    mdf_file.WriteHeaderToStream(file);

    // The CAN group is not a plain record group either.
    std::array<MdfValue, 1> values{0.0F};
    zassert_true(Throws<std::runtime_error>([&] { mdf_file.WriteDataRecordToStream(can_group, file, values); }));
}

ZTEST(mdf_file, test_TextBlocksAreSharedBetweenChannels) {
    Mdf4File mdf_file(1);

    auto first = mdf_file.GetOrCreateTextBlock("bar");
    auto second = mdf_file.GetOrCreateTextBlock("bar");
    auto other = mdf_file.GetOrCreateTextBlock("psi");

    zassert_equal(first, second, "identical texts must share one TX block");
    zassert_not_equal(first, other);
}

ZTEST(mdf_file, test_CanDlcFollowsTheIso11898Table) {
    const struct {
        size_t payload_size;
        uint8_t expected_dlc;
    } cases[] = {
        {0, 0}, {1, 1}, {8, 8}, {12, 9}, {16, 10}, {20, 11}, {24, 12}, {32, 13}, {48, 14}, {64, 15}};

    for(const auto& test_case : cases) {
        Mdf4File mdf_file(1);
        auto vlsd_channel_group = mdf_file.CreateVLSDChannelGroup(1);
        auto can_group = mdf_file.CreateCanDataFrameChannelGroup(vlsd_channel_group, 2, "Raw CAN Frame");

        auto file = MakeStream();
        const auto header_bytes_written = mdf_file.WriteHeaderToStream(file);

        mdf_file.WriteCanbusDataRecordToStream(can_group, file, MakeCanFrame(test_case.payload_size), 0.0F);

        const auto file_bytes = file.str();
        const auto data = AsBytes(file_bytes);
        const auto record = header_bytes_written + 1;

        zassert_equal((data[record + 8] >> 1) & 0x7FU, test_case.payload_size,
            "data length for a %zu byte payload", test_case.payload_size);
        zassert_equal((data[record + 9] >> 2) & 0xFU, test_case.expected_dlc,
            "DLC for a %zu byte payload", test_case.payload_size);
    }
}

ZTEST(mdf_file, test_RejectsAnOversizedCanPayload) {
    Mdf4File mdf_file(1);
    auto vlsd_channel_group = mdf_file.CreateVLSDChannelGroup(1);
    auto can_group = mdf_file.CreateCanDataFrameChannelGroup(vlsd_channel_group, 2, "Raw CAN Frame");

    auto file = MakeStream();
    mdf_file.WriteHeaderToStream(file);

    auto can_frame = MakeCanFrame(65);
    zassert_true(Throws<std::runtime_error>([&] {
        mdf_file.WriteCanbusDataRecordToStream(can_group, file, can_frame, 0.0F);
    }), "a payload larger than a CAN FD frame has no DLC");

    zassert_equal(can_group->GetCycleCount(), 0);
}

ZTEST(mdf_file, test_ExtendedAndStandardIdsAreMaskedToTheirWidth) {
    Mdf4File mdf_file(1);
    auto vlsd_channel_group = mdf_file.CreateVLSDChannelGroup(1);
    auto can_group = mdf_file.CreateCanDataFrameChannelGroup(vlsd_channel_group, 2, "Raw CAN Frame");

    auto file = MakeStream();
    const auto header_bytes_written = mdf_file.WriteHeaderToStream(file);

    // Values wider than the field must not spill into the neighbouring IDE bit.
    auto standard = MakeCanFrame(1);
    standard.id = 0xFFFFFFFF;
    standard.is_extended = false;
    mdf_file.WriteCanbusDataRecordToStream(can_group, file, standard, 0.0F);

    auto extended = MakeCanFrame(1);
    extended.id = 0xFFFFFFFF;
    extended.is_extended = true;
    mdf_file.WriteCanbusDataRecordToStream(can_group, file, extended, 0.0F);

    const auto file_bytes = file.str();
    const auto data = AsBytes(file_bytes);
    const auto record_size = 1 + can_group->GetDataSizeBytes();
    const auto vlsd_record_size = 1 + 4 + 1;

    const auto standard_pack = Read<uint32_t>(data, header_bytes_written + 1 + 4);
    zassert_equal((standard_pack >> 2) & 0x1FFFFFFFU, 0x7FFU, "an 11 bit id is masked");
    zassert_equal(standard_pack >> 31, 0, "IDE stays clear");

    const auto extended_offset = header_bytes_written + record_size + vlsd_record_size + 1 + 4;
    const auto extended_pack = Read<uint32_t>(data, extended_offset);
    zassert_equal((extended_pack >> 2) & 0x1FFFFFFFU, 0x1FFFFFFFU, "a 29 bit id is masked");
    zassert_equal(extended_pack >> 31, 1, "IDE is set");
}

ZTEST(mdf_file, test_HeaderCanBeRewrittenForANewFile) {
    Mdf4File mdf_file(RECORD_ID_SIZE);

    auto channel_group = mdf_file.CreateChannelGroup(1, "pressure");
    mdf_file.CreateDataChannel(channel_group, MdfDataType::Float32, "value", "bar");

    auto vlsd_channel_group = mdf_file.CreateVLSDChannelGroup(2);
    auto can_group = mdf_file.CreateCanDataFrameChannelGroup(vlsd_channel_group, 3, "Raw CAN Frame");

    auto write_file = [&](std::stringbuf& stream, int record_count) {
        mdf_file.WriteHeaderToStream(stream);

        for(int i = 0; i < record_count; i++) {
            std::array<MdfValue, 2> values{static_cast<float>(i), static_cast<float>(i)};
            mdf_file.WriteDataRecordToStream(channel_group, stream, values);
            mdf_file.WriteCanbusDataRecordToStream(can_group, stream, MakeCanFrame(8), static_cast<float>(i));
        }

        mdf_file.FinalizeToStream(stream);
    };

    auto first = MakeStream();
    write_file(first, 3);

    auto second = MakeStream();
    write_file(second, 3);

    // Rotating to a new file must restart every counter, not continue the previous one.
    zassert_equal(first.str(), second.str(), "a rotated file must be byte identical for identical content");
    zassert_equal(channel_group->GetCycleCount(), 3);
    zassert_equal(can_group->GetCycleCount(), 3);
    zassert_equal(vlsd_channel_group->GetVlsdDataSizeBytes(), 3 * 8);

    auto third = MakeStream();
    write_file(third, 5);
    zassert_true(third.str().size() > first.str().size(), "more records must produce a larger file");
}

ZTEST(mdf_file, test_FinalizeRequiresAWrittenHeader) {
    Mdf4File mdf_file(1);
    mdf_file.CreateChannelGroup(1, "pressure");

    auto file = MakeStream();
    zassert_true(Throws<std::runtime_error>([&] { mdf_file.FinalizeToStream(file); }));
}

ZTEST(mdf_file, test_FinalizeNeedsASeekableStream) {
    Mdf4File mdf_file(1);
    auto channel_group = mdf_file.CreateChannelGroup(1, "pressure");

    NonSeekableStreamBuf stream;
    mdf_file.WriteHeaderToStream(stream);

    std::array<MdfValue, 1> values{0.0F};
    mdf_file.WriteDataRecordToStream(channel_group, stream, values);

    bool threw = false;
    try {
        mdf_file.FinalizeToStream(stream);
    } catch(const std::ios_base::failure&) {
        threw = true;
    }
    zassert_true(threw, "a stream that cannot seek leaves the file unfinalized instead of corrupt");
}

ZTEST(mdf_file, test_DataGroupIsSharedByEveryChannelGroup) {
    Mdf4File mdf_file(RECORD_ID_SIZE);

    auto data_group = mdf_file.GetDataGroup();
    zassert_not_null(data_group.get());
    zassert_equal(data_group->GetRecordIdSizeBytes(), RECORD_ID_SIZE);
    zassert_equal(mdf_file.GetDataGroup(), data_group, "a file has exactly one data group");

    mdf_file.CreateChannelGroup(1, "pressure");
    mdf_file.CreateChannelGroup(2, "temperature");

    auto file = MakeStream();
    const auto header_bytes_written = mdf_file.WriteHeaderToStream(file);

    // Everything hangs off the single data group, whose DT block closes the header.
    const auto file_bytes = file.str();
    const auto data = AsBytes(file_bytes);
    zassert_equal(FindDataBlockAddress(data) + BLOCK_HEADER_SIZE, header_bytes_written);
    zassert_equal(mdf_file.GetDataGroup()->GetDataBlock()->GetAddress() + BLOCK_HEADER_SIZE, header_bytes_written);
}

ZTEST(mdf_file, test_FileExtensionIsMf4) {
    zassert_equal(std::string(Mdf4File::LOG_DATA_FILE_EXTENSION), std::string("mf4"));
}
