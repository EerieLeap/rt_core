#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <vector>

#ifdef CONFIG_ARCH_POSIX
    #include <fstream>
#endif

#include <zephyr/ztest.h>

#include "subsys/mdf/mdf4/channel_group_block.h"
#include "subsys/mdf/mdf4/data_group_block.h"
#include "subsys/mdf/mdf4/source_information_block.h"
#include "subsys/mdf/mdf4_file.h"
#include "subsys/mdf/mdf_data_type.h"
#include "subsys/mdf/mdf_property.h"
#include "subsys/mdf/mdf_value.h"
#include "subsys/time/rtc_provider.h"

#include "fixtures/golden_file.h"
#include "mdf_test_support.h"

using namespace eerie_leap::subsys::time;
using namespace eerie_leap::subsys::mdf;
using namespace mdf_test;
using eerie_leap::subsys::canbus::CanFrame;

namespace {

constexpr uint8_t RECORD_ID_SIZE = 1;
constexpr size_t ID_STANDARD_FLAGS_OFFSET = 60;

// Default layout: Float64 master time, then the 15 byte CAN_DataFrame with an 8 byte VLSD offset.
constexpr size_t TIME_SIZE = 8;
constexpr size_t CAN_FRAME_SIZE = 15;
constexpr size_t CAN_RECORD_SIZE = RECORD_ID_SIZE + TIME_SIZE + CAN_FRAME_SIZE;
constexpr size_t VLSD_LENGTH_SIZE = 4;

// A CAN frame goes out as its VLSD payload record (id, 4 byte length, payload), then the frame record.
constexpr size_t VlsdRecordSize(size_t payload_size) {
    return RECORD_ID_SIZE + VLSD_LENGTH_SIZE + payload_size;
}

// Member positions inside the record data, after the record id.
constexpr size_t CAN_BUS_CHANNEL = TIME_SIZE + 0;
constexpr size_t CAN_ID = TIME_SIZE + 1;
constexpr size_t CAN_FLAGS = TIME_SIZE + 5;
constexpr size_t CAN_DATA_LENGTH = TIME_SIZE + 6;
constexpr size_t CAN_DATA_BYTES = TIME_SIZE + 7;

constexpr size_t HD_FILE_HISTORY_LINK = 1;
constexpr size_t HD_COMMENT_LINK = 5;
constexpr size_t FH_COMMENT_LINK = 1;
constexpr size_t CG_COMMENT_LINK = 5;
constexpr size_t CN_NAME_LINK = 2;
constexpr size_t CN_COMMENT_LINK = 7;
constexpr size_t CN_FIELDS_OFFSET = BLOCK_LINKS_OFFSET + 8 * LINK_SIZE;
constexpr size_t CG_FIELDS_OFFSET = BLOCK_LINKS_OFFSET + 6 * LINK_SIZE;
constexpr size_t CG_CYCLE_COUNT = CG_FIELDS_OFFSET + 8;
constexpr size_t CG_DATA_BYTES = CG_FIELDS_OFFSET + 24;
constexpr size_t CG_INVAL_BYTES = CG_FIELDS_OFFSET + 28;

struct ChannelFields {
    uint8_t type;
    uint8_t sync_type;
    uint8_t data_type;
    uint8_t bit_offset;
    uint32_t byte_offset;
    uint32_t bit_count;
    uint32_t flags;
    uint32_t invalidation_bit_pos;
};

size_t ReadLink(std::span<const uint8_t> data, size_t block_address, size_t link_index) {
    return static_cast<size_t>(Read<uint64_t>(data, block_address + BLOCK_LINKS_OFFSET + link_index * LINK_SIZE));
}

// Text of a TX or MD block up to its zero terminator.
std::string ReadBlockText(std::span<const uint8_t> data, size_t block_address) {
    const auto length = static_cast<size_t>(Read<uint64_t>(data, block_address + BLOCK_LENGTH_OFFSET));
    const auto text = AsText(data, block_address + BLOCK_HEADER_SIZE, length - BLOCK_HEADER_SIZE);

    return text.substr(0, text.find('\0'));
}

ChannelFields ReadChannelFields(std::span<const uint8_t> data, size_t block_address) {
    zassert_equal(BlockId(data, block_address), std::string("##CN"), "no channel block at %zu", block_address);
    const auto base = block_address + CN_FIELDS_OFFSET;

    return ChannelFields{ .type = data[base],
        .sync_type = data[base + 1],
        .data_type = data[base + 2],
        .bit_offset = data[base + 3],
        .byte_offset = Read<uint32_t>(data, base + 4),
        .bit_count = Read<uint32_t>(data, base + 8),
        .flags = Read<uint32_t>(data, base + 12),
        .invalidation_bit_pos = Read<uint32_t>(data, base + 16) };
}

// The CAN_DataFrame members hang off the composite channel's cn_composition link, in record order.
std::vector<std::shared_ptr<mdf4::ChannelBlock>>
CanFrameMembers(const std::shared_ptr<mdf4::ChannelGroupBlock>& group) {
    const auto channels = group->GetChannels();
    zassert_equal(channels.size(), 2, "a CAN group has the master channel and the composite frame channel");

    std::vector<std::shared_ptr<mdf4::ChannelBlock>> members;
    auto member = std::dynamic_pointer_cast<mdf4::ChannelBlock>(channels[1]->GetChildren()[5]);
    while(member) {
        members.push_back(member);
        member = member->GetLinkedChannel();
    }

    return members;
}

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
    return CanFrame{ .id = 0x100,
        .is_extended = false,
        .is_transmit = false,
        .is_can_fd = payload_size > 8,
        .is_bitrate_switch = false,
        .data = std::vector<uint8_t>(payload_size, 0x5A) };
}

// Every kind of block and record the writer produces, with nothing that depends on the clock or the
// platform, so the bytes are the same on every target and every run.
void WriteGoldenFile(std::streambuf& stream) {
    Mdf4File mdf_file;
    mdf_file.UpdateCurrentTime(std::chrono::system_clock::time_point(std::chrono::seconds(1'760'000'000)));
    mdf_file.SetFileHistory("rt_core unit test", "Eerie Leap", "1.0.0");

    const std::array<MdfProperty, 2> properties{ { { "device_name", "golden <bench> & co" }, { "product_id", "42" } } };
    mdf_file.SetHeaderComment("rt_core golden file", properties);

    const auto source_information = std::make_shared<mdf4::SourceInformationBlock>(
        mdf4::SourceInformationBlock::SourceType::IoDevice, mdf4::SourceInformationBlock::BusType::None
    );
    source_information->SetName(mdf_file.GetOrCreateTextBlock("Eerie Leap Sensor"));

    const auto pressure_group = mdf_file.CreateChannelGroup(1, "oil_pressure");
    pressure_group->AddSourceInformation(source_information);
    const auto pressure_channel = mdf_file.CreateDataChannel(
        pressure_group, MdfDataType::Float32, "oil_pressure", "bar"
    );
    pressure_channel->SetConversion(mdf_file.CreateAlgebraicConversion("X * 0.1"));
    mdf_file.SetChannelComment(pressure_channel, "Oil pressure: main gallery");

    const auto coolant_group = mdf_file.CreateChannelGroup(2, "coolant");
    coolant_group->AddSourceInformation(source_information);
    mdf_file.CreateDataChannel(coolant_group, MdfDataType::Float32, "coolant_temperature", "C");
    mdf_file.CreateDataChannel(coolant_group, MdfDataType::Int32, "coolant_counts", "");

    const auto vlsd_group = mdf_file.CreateVLSDChannelGroup(3);
    const auto can_group = mdf_file.CreateCanDataFrameChannelGroup(vlsd_group, 4, "can0");

    mdf_file.WriteHeaderToStream(stream);

    CanFrame classic{ .id = 0x123,
        .is_extended = false,
        .is_transmit = false,
        .is_can_fd = false,
        .is_bitrate_switch = false,
        .data = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 } };

    CanFrame fd_12{ .id = 0x18FF1234,
        .is_extended = true,
        .is_transmit = false,
        .is_can_fd = true,
        .is_bitrate_switch = true,
        .data = std::vector<uint8_t>(12, 0xAB) };

    CanFrame fd_64 = MakeCanFrame(64);
    fd_64.id = 0x7FF;
    fd_64.is_transmit = true;

    const std::array<const CanFrame*, 3> frames{ &classic, &fd_12, &fd_64 };

    for(uint32_t i = 0; i < 3; i++) {
        const double time = static_cast<double>(i) * 0.25;

        std::array<MdfValue, 2> pressure{ time, 1000.0F + static_cast<float>(i) * 10.0F };
        mdf_file.WriteDataRecordToStream(pressure_group, stream, pressure);

        mdf_file.WriteCanbusDataRecordToStream(can_group, stream, *frames[i], time + 0.125, 1);

        std::array<MdfValue, 3> coolant{ time + 0.0625, 80.0F + static_cast<float>(i) * 0.5F,
            static_cast<int32_t>(i) - 1 };
        mdf_file.WriteDataRecordToStream(coolant_group, stream, coolant);
    }

    mdf_file.FinalizeToStream(stream);
}

} // namespace

ZTEST_SUITE(mdf_file, NULL, NULL, NULL, NULL, NULL);

// The writer's output is pinned byte for byte: refactors must not move a single byte, and a format
// change has to regenerate the fixture on purpose (see fixtures/golden_file.h).
ZTEST(mdf_file, test_OutputMatchesTheGoldenFile) {
    auto file = MakeStream();
    WriteGoldenFile(file);

    const auto file_bytes = file.str();

#ifdef CONFIG_ARCH_POSIX
    const std::string_view dump_dir = CONFIG_MDF_TEST_DUMP_DIR;
    if(!dump_dir.empty()) {
        const std::string path = std::string(dump_dir) + "/golden_file.mf4";
        std::ofstream dump(path, std::ios::binary | std::ios::trunc);
        dump.write(file_bytes.data(), static_cast<std::streamsize>(file_bytes.size()));
        zassert_true(dump.good(), "writing %s failed", path.c_str());
    }
#endif

    zassert_equal(
        file_bytes.size(), sizeof(GOLDEN_FILE), "the file is %zu bytes, the golden file %zu", file_bytes.size(),
        sizeof(GOLDEN_FILE)
    );

    for(size_t i = 0; i < file_bytes.size(); i++) {
        zassert_equal(
            static_cast<uint8_t>(file_bytes[i]), GOLDEN_FILE[i],
            "first difference at byte %zu: 0x%02x, the golden file has 0x%02x", i,
            static_cast<unsigned>(static_cast<uint8_t>(file_bytes[i])), static_cast<unsigned>(GOLDEN_FILE[i])
        );
    }
}

// Runs on the default test stack: writing, laying out and destroying 1000 channel groups must not
// nest a call per group or per channel. Only the QEMU targets can catch a regression here: native_sim
// runs Zephyr threads on large host stacks, where the old recursive code passed this test too.
ZTEST(mdf_file, test_DeepChainsSerializeAndDestroyIteratively) {
    constexpr size_t group_count = 1000;

    std::vector<uint64_t> group_addresses;
    std::vector<uint64_t> channel_addresses;
    std::string file_bytes;

    {
        Mdf4File mdf_file({ .record_id_size_bytes = 2 });

        std::vector<std::shared_ptr<mdf4::ChannelGroupBlock>> groups;
        for(size_t i = 0; i < group_count; i++) {
            const auto group = mdf_file.CreateChannelGroup(i + 1, "group_" + std::to_string(i));
            mdf_file.CreateDataChannel(group, MdfDataType::Float32, "value", "bar");
            groups.push_back(group);
        }

        auto file = MakeStream();
        const auto header_bytes_written = mdf_file.WriteHeaderToStream(file);
        file_bytes = file.str();
        zassert_equal(file_bytes.size(), header_bytes_written, "the header is everything that was written");

        for(const auto& group : groups) {
            group_addresses.push_back(group->GetAddress());
            for(const auto& channel : group->GetChannels())
                channel_addresses.push_back(channel->GetAddress());
        }

        // Only the file owns the blocks now, so its destruction frees the whole chain.
    }

    const auto data = AsBytes(file_bytes);
    zassert_equal(group_addresses.size(), group_count);
    zassert_equal(
        channel_addresses.size(), 2 * group_count, "every group has the master channel and one value channel"
    );

    for(size_t i = 0; i < group_addresses.size(); i++) {
        zassert_equal(
            BlockId(data, static_cast<size_t>(group_addresses[i])), std::string("##CG"),
            "group %zu is not at its address", i
        );
    }

    for(size_t i = 0; i < channel_addresses.size(); i++) {
        zassert_equal(
            BlockId(data, static_cast<size_t>(channel_addresses[i])), std::string("##CN"),
            "channel %zu is not at its address", i
        );
    }

    // A chain built outside a file is released by its head alone.
    auto data_group = std::make_shared<mdf4::DataGroupBlock>(2);
    std::weak_ptr<mdf4::ChannelGroupBlock> last_group;
    for(size_t i = 0; i < group_count; i++) {
        auto group = std::make_shared<mdf4::ChannelGroupBlock>(2, i + 1);
        last_group = group;
        data_group->AddChannelGroup(std::move(group));
    }

    data_group.reset();
    zassert_true(last_group.expired(), "destroying the data group frees the end of its channel group chain");
}

// Each CAN group's DataBytes channel points back at its VLSD group, which is also on the chain.
ZTEST(mdf_file, test_DestroyingTheFileFreesEveryBlock) {
    std::weak_ptr<mdf4::ChannelGroupBlock> value_group;
    std::weak_ptr<mdf4::ChannelGroupBlock> vlsd_group;
    std::weak_ptr<mdf4::ChannelGroupBlock> can_group;
    std::weak_ptr<mdf4::ChannelGroupBlock> last_group;

    {
        Mdf4File mdf_file;
        value_group = mdf_file.CreateChannelGroup(1, "pressure");
        vlsd_group = mdf_file.CreateVLSDChannelGroup(2);
        can_group = mdf_file.CreateCanDataFrameChannelGroup(vlsd_group.lock(), 3, "can0");
        last_group = mdf_file.CreateChannelGroup(4, "temperature");

        auto file = MakeStream();
        mdf_file.WriteHeaderToStream(file);
    }

    zassert_true(value_group.expired(), "the value group outlived its file");
    zassert_true(vlsd_group.expired(), "the VLSD group outlived its file");
    zassert_true(can_group.expired(), "the CAN group outlived its file");
    zassert_true(last_group.expired(), "a group after the VLSD group outlived its file");
}

ZTEST(mdf_file, test_WriteAndFinalize) {
    RtcProvider rtc_time_provider;

    Mdf4File mdf_file({ .record_id_size_bytes = RECORD_ID_SIZE });
    mdf_file.UpdateCurrentTime(rtc_time_provider.GetTime());

    const auto channel_group_1 = mdf_file.CreateChannelGroup(1, "pressure");
    const auto source_information = std::make_shared<mdf4::SourceInformationBlock>(
        mdf4::SourceInformationBlock::SourceType::IoDevice, mdf4::SourceInformationBlock::BusType::None
    );
    source_information->SetName(mdf_file.GetOrCreateTextBlock("Eerie Leap Sensor"));
    channel_group_1->AddSourceInformation(source_information);

    const auto channel_1 = mdf_file.CreateDataChannel(channel_group_1, MdfDataType::Uint64, "value", "bar");
    channel_1->SetConversion(mdf_file.CreateAlgebraicConversion("x * 0.1"));

    const auto channel_group_2 = mdf_file.CreateChannelGroup(2, "temperature");
    channel_group_2->AddSourceInformation(source_information);
    mdf_file.CreateDataChannel(channel_group_2, MdfDataType::Float32, "value", "C");

    const auto vlsd_channel_group_3 = mdf_file.CreateVLSDChannelGroup(3);
    const auto channel_group_3 = mdf_file.CreateCanDataFrameChannelGroup(vlsd_channel_group_3, 4, "Raw CAN Frame");

    MemoryStreamBuf file;

    const auto header_bytes_written = mdf_file.WriteHeaderToStream(file);
    zassert_true(header_bytes_written > ID_BLOCK_SIZE);

    constexpr uint64_t value_record_count = 10;
    for(uint64_t i = 0; i < value_record_count; i++) {
        std::array<MdfValue, 2> values{ static_cast<double>(i) * 0.1, i * i };
        mdf_file.WriteDataRecordToStream(channel_group_1, file, values);
    }

    for(uint64_t i = 0; i < value_record_count; i++) {
        std::array<MdfValue, 2> values{ static_cast<double>(i) * 0.1, static_cast<float>(i) };
        mdf_file.WriteDataRecordToStream(channel_group_2, file, values);
    }

    constexpr uint64_t can_record_count = 10;
    constexpr size_t can_payload_size = 8;
    for(uint64_t i = 0; i < can_record_count; i++) {
        CanFrame can_frame{ .id = 0x18FF1234,
            .is_extended = true,
            .is_transmit = false,
            .is_can_fd = false,
            .is_bitrate_switch = false,
            .data = { 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0 } };

        mdf_file.WriteCanbusDataRecordToStream(channel_group_3, file, can_frame, static_cast<double>(i) * 0.1, 1);
    }

    zassert_equal(channel_group_1->GetCycleCount(), value_record_count);
    zassert_equal(channel_group_2->GetCycleCount(), value_record_count);
    zassert_equal(channel_group_3->GetCycleCount(), can_record_count);
    zassert_equal(vlsd_channel_group_3->GetCycleCount(), can_record_count);
    zassert_equal(vlsd_channel_group_3->GetVlsdDataSizeBytes(), can_record_count * can_payload_size);

    mdf_file.FinalizeToStream(file);

    const auto file_bytes = file.str();
    const auto data = AsBytes(file_bytes);

    // File identifier is switched to finalized and the unfinalized flags are cleared.
    zassert_mem_equal(data.data(), "MDF     ", 8);
    zassert_mem_equal(data.data() + 8, "4.10    ", 8);
    zassert_equal(Read<uint32_t>(data, ID_STANDARD_FLAGS_OFFSET), 0, "Unfinalized flags were not cleared");

    // The DT block length has to cover every record appended after the header.
    const auto data_block_address = FindDataBlockAddress(data);
    zassert_not_equal(data_block_address, 0, "DT block not found");
    zassert_equal(
        data_block_address + BLOCK_HEADER_SIZE, header_bytes_written, "DT block is not the last header block"
    );

    const uint64_t expected_record_bytes = value_record_count * (RECORD_ID_SIZE + channel_group_1->GetDataSizeBytes())
        + value_record_count * (RECORD_ID_SIZE + channel_group_2->GetDataSizeBytes())
        + can_record_count * (RECORD_ID_SIZE + channel_group_3->GetDataSizeBytes())
        + can_record_count * (RECORD_ID_SIZE + VLSD_LENGTH_SIZE + can_payload_size);

    zassert_equal(RECORD_ID_SIZE + channel_group_3->GetDataSizeBytes(), CAN_RECORD_SIZE);

    zassert_equal(
        Read<uint64_t>(data, data_block_address + BLOCK_LENGTH_OFFSET), BLOCK_HEADER_SIZE + expected_record_bytes
    );
    zassert_equal(data.size(), header_bytes_written + expected_record_bytes);
}

ZTEST(mdf_file, test_CanFdFrameEncoding) {
    Mdf4File mdf_file;

    const auto vlsd_channel_group = mdf_file.CreateVLSDChannelGroup(1);
    const auto channel_group = mdf_file.CreateCanDataFrameChannelGroup(vlsd_channel_group, 2, "Raw CAN Frame");

    MemoryStreamBuf file;
    const auto header_bytes_written = mdf_file.WriteHeaderToStream(file);

    CanFrame can_frame{ .id = 0x7FF,
        .is_extended = false,
        .is_transmit = true,
        .is_can_fd = true,
        .is_bitrate_switch = true,
        .data = std::vector<uint8_t>(12, 0xAA) };

    mdf_file.WriteCanbusDataRecordToStream(channel_group, file, can_frame, 1.5, 2);
    mdf_file.FinalizeToStream(file);

    const auto file_bytes = file.str();
    const auto data = AsBytes(file_bytes);
    const size_t record = header_bytes_written + VlsdRecordSize(12) + RECORD_ID_SIZE;

    zassert_equal(data[record - RECORD_ID_SIZE], 2, "the frame record follows its payload");
    zassert_equal(Read<double>(data, record), 1.5, "the master time is a Float64");
    zassert_equal(data[record + CAN_BUS_CHANNEL], 2, "bus channel is written as given");

    const auto id_pack = Read<uint32_t>(data, record + CAN_ID);
    zassert_equal(id_pack & 0x1FFFFFFFU, 0x7FFU, "frame id");
    zassert_equal((id_pack >> 29) & 0x3U, 0, "the two bits between ID and IDE stay clear");
    zassert_equal((id_pack >> 31) & 0x1U, 0, "IDE must be clear for a standard id");

    const auto flags = static_cast<uint8_t>(data[record + CAN_FLAGS]);
    zassert_equal(flags & 0xFU, 9, "DLC code for a 12 byte CAN FD payload");
    zassert_equal((flags >> 4) & 0x1U, 1, "direction");
    zassert_equal((flags >> 5) & 0x1U, 1, "EDL");
    zassert_equal((flags >> 6) & 0x1U, 1, "BRS");
    zassert_equal((flags >> 7) & 0x1U, 0, "ESI is never set");

    zassert_equal(data[record + CAN_DATA_LENGTH], 12, "data length");
    zassert_equal(Read<uint64_t>(data, record + CAN_DATA_BYTES), 0, "VLSD offset of the first frame");
    zassert_equal(vlsd_channel_group->GetVlsdDataSizeBytes(), 12);

    // The VLSD record comes first: id, 4 byte length, payload.
    const size_t vlsd_record = header_bytes_written;
    zassert_equal(data[vlsd_record], 1, "VLSD record id");
    zassert_equal(Read<uint32_t>(data, vlsd_record + RECORD_ID_SIZE), 12, "VLSD length prefix");
    zassert_equal(data[vlsd_record + RECORD_ID_SIZE + VLSD_LENGTH_SIZE], 0xAA);
}

// A file cut anywhere must not leave a complete frame record pointing at a payload that is not
// in the file: readers would fail the whole CAN group on it.
ZTEST(mdf_file, test_VlsdPayloadIsWrittenBeforeItsFrame) {
    Mdf4File mdf_file;
    const auto vlsd_channel_group = mdf_file.CreateVLSDChannelGroup(1);
    const auto can_group = mdf_file.CreateCanDataFrameChannelGroup(vlsd_channel_group, 2, "Raw CAN Frame");

    auto file = MakeStream();
    const auto header_bytes_written = mdf_file.WriteHeaderToStream(file);

    const std::array<size_t, 3> payload_sizes{ 3, 64, 5 };
    for(const size_t payload_size : payload_sizes)
        mdf_file.WriteCanbusDataRecordToStream(can_group, file, MakeCanFrame(payload_size), 0.0, 1);

    const auto file_bytes = file.str();
    const auto data = AsBytes(file_bytes);

    size_t position = header_bytes_written;
    uint64_t vlsd_bytes_written = 0;

    for(size_t i = 0; i < payload_sizes.size(); i++) {
        zassert_equal(data[position], 1, "frame %zu: the VLSD record must come first", i);
        zassert_equal(Read<uint32_t>(data, position + RECORD_ID_SIZE), payload_sizes[i], "frame %zu: VLSD length", i);
        position += VlsdRecordSize(payload_sizes[i]);
        vlsd_bytes_written += VLSD_LENGTH_SIZE + payload_sizes[i];

        zassert_equal(data[position], 2, "frame %zu: the frame record must follow its payload", i);
        const auto offset = Read<uint64_t>(data, position + RECORD_ID_SIZE + CAN_DATA_BYTES);
        zassert_equal(
            offset + VLSD_LENGTH_SIZE + payload_sizes[i], vlsd_bytes_written,
            "frame %zu must point at the payload written just before it", i
        );
        position += CAN_RECORD_SIZE;
    }

    zassert_equal(position, data.size(), "nothing but the three record pairs may follow the header");
}

ZTEST(mdf_file, test_UnfinalizedFileIsMarkedAsSuch) {
    Mdf4File mdf_file;

    const auto channel_group = mdf_file.CreateChannelGroup(1, "pressure");
    mdf_file.CreateDataChannel(channel_group, MdfDataType::Float32, "value", "bar");

    MemoryStreamBuf file;
    mdf_file.WriteHeaderToStream(file);

    for(uint64_t i = 0; i < 4; i++) {
        std::array<MdfValue, 2> values{ static_cast<double>(i), static_cast<float>(i) };
        mdf_file.WriteDataRecordToStream(channel_group, file, values);
    }

    // Never finalized, as would happen after a power loss.
    const auto file_bytes = file.str();
    const auto data = AsBytes(file_bytes);
    zassert_mem_equal(data.data(), "UnFinMF ", 8);

    constexpr uint32_t expected_flags = 0x01 | 0x04 | 0x20;
    zassert_equal(
        Read<uint32_t>(data, ID_STANDARD_FLAGS_OFFSET) & 0xFFFFU, expected_flags, "unfinalized flags must be set"
    );
}

ZTEST(mdf_file, test_RejectsRecordIdThatDoesNotFit) {
    Mdf4File mdf_file({ .record_id_size_bytes = 1 });

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
    Mdf4File mdf_file;

    const auto channel_group = mdf_file.CreateChannelGroup(1, "pressure");
    mdf_file.CreateDataChannel(channel_group, MdfDataType::Uint64, "value", "bar");

    MemoryStreamBuf file;
    mdf_file.WriteHeaderToStream(file);

    bool threw = false;
    try {
        std::array<MdfValue, 2> wrong_type{ 0.0, 1.0F };
        mdf_file.WriteDataRecordToStream(channel_group, file, wrong_type);
    } catch(const std::runtime_error&) {
        threw = true;
    }
    zassert_true(threw, "a value type that does not match the channel should be rejected");

    threw = false;
    try {
        std::array<MdfValue, 2> wrong_time{ 0.0F, uint64_t{ 1 } };
        mdf_file.WriteDataRecordToStream(channel_group, file, wrong_time);
    } catch(const std::runtime_error&) {
        threw = true;
    }
    zassert_true(threw, "a Float64 master channel must refuse a float time");

    threw = false;
    try {
        std::array<MdfValue, 1> wrong_count{ 0.0 };
        mdf_file.WriteDataRecordToStream(channel_group, file, wrong_count);
    } catch(const std::runtime_error&) {
        threw = true;
    }
    zassert_true(threw, "a wrong number of values should be rejected");
}

ZTEST(mdf_file, test_RejectsZeroRecordIdSize) {
    bool threw = false;
    try {
        const Mdf4File mdf_file({ .record_id_size_bytes = 0 });
    } catch(const std::invalid_argument&) {
        threw = true;
    }
    zassert_true(threw, "records of several channel groups share one data block and need an id");

    for(uint8_t size : { 3, 5, 16 }) {
        zassert_true(
            Throws<std::invalid_argument>([&] {
                Mdf4File mdf_file({ .record_id_size_bytes = size });
            }),
            "record id size %u is not a valid MDF record id width", size
        );
    }

    for(const uint8_t size : { 1, 2, 4, 8 })
        zassert_equal(Mdf4File({ .record_id_size_bytes = size }).GetDataGroup()->GetRecordIdSizeBytes(), size);
}

ZTEST(mdf_file, test_RejectsUnsupportedTimeTypeAndOffsetWidth) {
    for(auto time_type : { MdfDataType::None, MdfDataType::Int32, MdfDataType::Uint64, MdfDataType::ByteArray }) {
        zassert_true(
            Throws<std::invalid_argument>([&] {
                Mdf4File mdf_file({ .time_type = time_type });
            }),
            "a master time channel of type %u has no meaning in seconds", static_cast<unsigned>(time_type)
        );
    }

    for(uint8_t width : { 0, 1, 2, 3, 16 }) {
        zassert_true(
            Throws<std::invalid_argument>([&] {
                Mdf4File mdf_file({ .vlsd_offset_size_bytes = width });
            }),
            "readers only accept 4 or 8 byte VLSD offsets, not %u", width
        );
    }
}

ZTEST(mdf_file, test_StructureIsFrozenOnceTheHeaderIsWritten) {
    Mdf4File mdf_file;
    auto channel_group = mdf_file.CreateChannelGroup(1, "pressure");

    auto file = MakeStream();
    mdf_file.WriteHeaderToStream(file);

    // Adding blocks now would invalidate every address already written to the stream.
    zassert_true(Throws<std::runtime_error>([&] {
        mdf_file.CreateChannelGroup(2, "late");
    }));
    zassert_true(Throws<std::runtime_error>([&] {
        mdf_file.CreateVLSDChannelGroup(3);
    }));
    zassert_true(Throws<std::runtime_error>([&] {
        mdf_file.CreateDataChannel(channel_group, MdfDataType::Float32, "late", "");
    }));
}

ZTEST(mdf_file, test_RejectsRecordsOutsideTheWritableWindow) {
    Mdf4File mdf_file;
    auto channel_group = mdf_file.CreateChannelGroup(1, "pressure");

    auto file = MakeStream();
    std::array<MdfValue, 1> values{ 0.0 };

    zassert_true(
        Throws<std::runtime_error>([&] {
            mdf_file.WriteDataRecordToStream(channel_group, file, values);
        }),
        "records written before the header would land in front of the DT block"
    );

    mdf_file.WriteHeaderToStream(file);
    mdf_file.WriteDataRecordToStream(channel_group, file, values);
    mdf_file.FinalizeToStream(file);

    zassert_true(
        Throws<std::runtime_error>([&] {
            mdf_file.WriteDataRecordToStream(channel_group, file, values);
        }),
        "records written after finalizing would not be covered by the patched DT length"
    );
}

ZTEST(mdf_file, test_RejectsChannelGroupsFromAnotherFile) {
    Mdf4File mdf_file;
    mdf_file.CreateChannelGroup(1, "pressure");

    Mdf4File other_file;
    auto foreign_group = other_file.CreateChannelGroup(1, "pressure");

    auto file = MakeStream();
    mdf_file.WriteHeaderToStream(file);

    std::array<MdfValue, 1> values{ 0.0 };
    zassert_true(Throws<std::runtime_error>([&] {
        mdf_file.WriteDataRecordToStream(foreign_group, file, values);
    }));

    auto can_frame = MakeCanFrame(8);
    zassert_true(Throws<std::runtime_error>([&] {
        mdf_file.WriteCanbusDataRecordToStream(foreign_group, file, can_frame, 0.0, 1);
    }));
}

ZTEST(mdf_file, test_CanDataFrameGroupRequiresAVlsdGroup) {
    Mdf4File mdf_file;

    zassert_true(
        Throws<std::invalid_argument>([&] {
            mdf_file.CreateCanDataFrameChannelGroup(nullptr, 1, "Raw CAN Frame");
        }),
        "a null VLSD group would leave the payload channel dangling"
    );

    auto plain_group = mdf_file.CreateChannelGroup(1, "plain");
    zassert_true(
        Throws<std::runtime_error>([&] {
            mdf_file.CreateCanDataFrameChannelGroup(plain_group, 2, "Raw CAN Frame");
        }),
        "a group without the VLSD flag cannot hold variable length payloads"
    );
}

ZTEST(mdf_file, test_CanDataFrameGroupRejectsExtraChannels) {
    Mdf4File mdf_file;

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
    std::array<MdfValue, 1> values{ 0.0 };
    zassert_true(Throws<std::runtime_error>([&] {
        mdf_file.WriteDataRecordToStream(can_group, file, values);
    }));
}

ZTEST(mdf_file, test_CreateDataChannelKeepsTheRecordWithinTheWriterLimits) {
    Mdf4File mdf_file;
    auto channel_group = mdf_file.CreateChannelGroup(1, "wide");

    // id 1 + time 8 + 6 * 8 = 57 bytes in 7 channels.
    for(int i = 0; i < 6; i++)
        mdf_file.CreateDataChannel(channel_group, MdfDataType::Float64, "value_" + std::to_string(i), "");

    std::string message;
    try {
        mdf_file.CreateDataChannel(channel_group, MdfDataType::Float64, "too_wide", "");
    } catch(const std::runtime_error& error) {
        message = error.what();
    }
    zassert_true(
        message.find("65") != std::string::npos,
        "a 65 byte record must be rejected with its size in the message (got \"%s\")", message.c_str()
    );
    zassert_equal(channel_group->GetChannels().size(), 7, "a rejected channel is not added");

    // 61 bytes in the eighth and last channel the record writer has a field for.
    mdf_file.CreateDataChannel(channel_group, MdfDataType::Float32, "last", "");
    zassert_true(
        Throws<std::runtime_error>([&] {
            mdf_file.CreateDataChannel(channel_group, MdfDataType::Int32, "ninth", "");
        }),
        "a ninth channel exceeds the record writer's field table"
    );

    auto file = MakeStream();
    const auto header_bytes_written = mdf_file.WriteHeaderToStream(file);
    zassert_equal(mdf_file.GetRecordWriter(channel_group).GetRecordSizeBytes(), 61);
    zassert_true(header_bytes_written > 0);
}

ZTEST(mdf_file, test_CreateDataChannelCountsTheInvalidationByte) {
    Mdf4File mdf_file({ .record_id_size_bytes = 8 });

    // id 8 + time 8 + 6 * 8 = 64 bytes: the largest record, as long as it has no invalidation byte.
    const auto without_bits = mdf_file.CreateChannelGroup(1, "without_bits");
    for(int i = 0; i < 5; i++)
        mdf_file.CreateDataChannel(without_bits, MdfDataType::Float64, "value_" + std::to_string(i), "");

    std::string message;
    try {
        mdf_file.CreateDataChannel(without_bits, MdfDataType::Float64, "invalidated", "", true);
    } catch(const std::runtime_error& error) {
        message = error.what();
    }
    zassert_true(
        message.find("65") != std::string::npos,
        "the invalidation byte must count towards the record size (got \"%s\")", message.c_str()
    );
    zassert_equal(
        without_bits->GetInvalidationBytes(), 0, "a rejected channel must not leave an invalidation byte behind"
    );
    zassert_equal(without_bits->GetChannels().size(), 6, "a rejected channel is not added");
    mdf_file.CreateDataChannel(without_bits, MdfDataType::Float64, "last", "");

    // Once the group has its invalidation byte, a channel without a bit has one byte less room too.
    auto with_bits = mdf_file.CreateChannelGroup(2, "with_bits");
    mdf_file.CreateDataChannel(with_bits, MdfDataType::Float64, "invalidated", "", true);
    for(int i = 0; i < 4; i++)
        mdf_file.CreateDataChannel(with_bits, MdfDataType::Float64, "value_" + std::to_string(i), "");
    zassert_true(
        Throws<std::runtime_error>([&] {
            mdf_file.CreateDataChannel(with_bits, MdfDataType::Float64, "too_wide", "");
        }),
        "8 + 8 + 6 * 8 data bytes and the invalidation byte make 65 bytes"
    );
    const auto last_bit = mdf_file.CreateDataChannel(with_bits, MdfDataType::Float32, "last", "", true);
    zassert_equal(last_bit->GetInvalidationBitPosition(), 1, "a group's bits are handed out in channel order");

    // The master channel takes one of the eight fields, so seven channels with a bit is the most a
    // group of this file can have; the record writer tests cover the eight bits of the byte.
    auto seven_bits = mdf_file.CreateChannelGroup(3, "seven_bits");
    for(uint32_t i = 0; i < 7; i++) {
        const auto channel = mdf_file.CreateDataChannel(
            seven_bits, MdfDataType::Float32, "bit_" + std::to_string(i), "", true
        );
        zassert_equal(channel->GetInvalidationBitPosition(), i, "channel %u has bit %u", i, i);
    }
    zassert_true(
        Throws<std::runtime_error>([&] {
            mdf_file.CreateDataChannel(seven_bits, MdfDataType::Float32, "eighth", "", true);
        }),
        "an eighth data channel exceeds the record writer's field table"
    );
    zassert_equal(seven_bits->GetInvalidationBytes(), 1, "seven bits share one invalidation byte");
    zassert_false(seven_bits->GetChannels().at(0)->HasInvalidationBit(), "the master channel never has a bit");

    auto file = MakeStream();
    mdf_file.WriteHeaderToStream(file);
    zassert_equal(mdf_file.GetRecordWriter(without_bits).GetRecordSizeBytes(), 64);
    zassert_equal(mdf_file.GetRecordWriter(with_bits).GetRecordSizeBytes(), 8 + 8 + 5 * 8 + 4 + 1);
    zassert_equal(mdf_file.GetRecordWriter(seven_bits).GetRecordSizeBytes(), 8 + 8 + 7 * 4 + 1);
}

// Pins what invalidation bits add, without a golden file: cg_inval_bytes, each channel's flag and
// bit position, and the byte after the data bytes of every record, which the DT length covers.
ZTEST(mdf_file, test_InvalidationBitsLayoutAndRecords) {
    Mdf4File mdf_file;
    const auto group = mdf_file.CreateChannelGroup(1, "oil_pressure");
    const auto value = mdf_file.CreateDataChannel(group, MdfDataType::Float32, "oil_pressure", "bar", true);
    const auto raw = mdf_file.CreateDataChannel(group, MdfDataType::Float32, "oil_pressure.raw", "", true);
    const auto counts = mdf_file.CreateDataChannel(group, MdfDataType::Int32, "counts", "");
    const auto vlsd_group = mdf_file.CreateVLSDChannelGroup(2);
    const auto can_group = mdf_file.CreateCanDataFrameChannelGroup(vlsd_group, 3, "can0");

    zassert_equal(group->GetDataSizeBytes(), TIME_SIZE + 3 * 4, "the invalidation byte is not a data byte");

    auto file = MakeStream();
    const auto header_bytes_written = mdf_file.WriteHeaderToStream(file);

    constexpr size_t record_size = RECORD_ID_SIZE + TIME_SIZE + 3 * 4 + 1;
    zassert_equal(mdf_file.GetRecordWriter(group).GetRecordSizeBytes(), record_size);

    const std::array<MdfValue, 4> raw_invalid{ 0.5, 1.5F, 0.0F, int32_t{ -2 } };
    const std::array<MdfValue, 4> value_invalid{ 0.75, 0.0F, 2.0F, int32_t{ 7 } };
    const std::array<MdfValue, 4> both_invalid{ 1.0, 0.0F, 0.0F, int32_t{ 0 } };
    const std::array<MdfValue, 4> all_valid{ 1.25, 3.0F, 4.0F, int32_t{ 1 } };
    mdf_file.WriteDataRecordToStream(group, file, raw_invalid, 0b10);
    mdf_file.WriteDataRecordToStream(group, file, value_invalid, 0b01);
    mdf_file.WriteDataRecordToStream(group, file, both_invalid, 0b11);
    mdf_file.WriteDataRecordToStream(group, file, all_valid);
    mdf_file.FinalizeToStream(file);

    const std::array<uint8_t, 4 * record_size> expected{ 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE0, 0x3F, 0x00,
        0x00, 0xC0, 0x3F, 0x00, 0x00, 0x00, 0x00, 0xFE, 0xFF, 0xFF, 0xFF, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0xE8, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x07, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF4, 0x3F, 0x00, 0x00, 0x40, 0x40, 0x00, 0x00, 0x80,
        0x40, 0x01, 0x00, 0x00, 0x00, 0x00 };

    const auto file_bytes = file.str();
    const auto data = AsBytes(file_bytes);
    zassert_equal(
        data.size(), header_bytes_written + expected.size(), "four records of %zu bytes follow the header", record_size
    );
    for(size_t i = 0; i < expected.size(); i++) {
        zassert_equal(
            data[header_bytes_written + i], expected[i], "record byte %zu is 0x%02x, expected 0x%02x", i,
            static_cast<unsigned>(data[header_bytes_written + i]), static_cast<unsigned>(expected[i])
        );
    }

    const auto data_block_address = FindDataBlockAddress(data);
    zassert_equal(
        Read<uint64_t>(data, data_block_address + BLOCK_LENGTH_OFFSET), BLOCK_HEADER_SIZE + expected.size(),
        "the DT length covers the invalidation bytes"
    );

    const auto group_address = static_cast<size_t>(group->GetAddress());
    zassert_equal(BlockId(data, group_address), std::string("##CG"));
    zassert_equal(Read<uint64_t>(data, group_address + CG_CYCLE_COUNT), 4, "cg_cycle_count");
    zassert_equal(Read<uint32_t>(data, group_address + CG_DATA_BYTES), TIME_SIZE + 3 * 4, "cg_data_bytes");
    zassert_equal(Read<uint32_t>(data, group_address + CG_INVAL_BYTES), 1, "cg_inval_bytes");
    zassert_equal(
        Read<uint32_t>(data, static_cast<size_t>(can_group->GetAddress()) + CG_INVAL_BYTES), 0,
        "a CAN group has no invalidation bytes"
    );

    const auto invalid_valid = std::to_underlying(mdf4::ChannelBlock::Flag::InvalidValid);
    const auto master_fields = ReadChannelFields(data, static_cast<size_t>(group->GetChannels().at(0)->GetAddress()));
    zassert_equal(master_fields.flags & invalid_valid, 0, "the master channel never has an invalidation bit");
    zassert_equal(master_fields.invalidation_bit_pos, 0);

    const auto value_fields = ReadChannelFields(data, static_cast<size_t>(value->GetAddress()));
    zassert_equal(value_fields.flags, invalid_valid, "cn_flags of the value channel");
    zassert_equal(value_fields.invalidation_bit_pos, 0, "cn_inval_bit_pos of the value channel");

    const auto raw_fields = ReadChannelFields(data, static_cast<size_t>(raw->GetAddress()));
    zassert_equal(raw_fields.flags, invalid_valid, "cn_flags of the raw channel");
    zassert_equal(raw_fields.invalidation_bit_pos, 1, "cn_inval_bit_pos of the raw channel");

    const auto counts_fields = ReadChannelFields(data, static_cast<size_t>(counts->GetAddress()));
    zassert_equal(counts_fields.flags, 0, "a channel created without a bit has no InvalidValid");
    zassert_equal(counts_fields.invalidation_bit_pos, 0);
}

ZTEST(mdf_file, test_RecordWritersAreBuiltWithTheHeader) {
    Mdf4File mdf_file;
    auto channel_group = mdf_file.CreateChannelGroup(1, "pressure");
    mdf_file.CreateDataChannel(channel_group, MdfDataType::Float32, "value", "bar");
    auto vlsd_channel_group = mdf_file.CreateVLSDChannelGroup(2);
    auto can_group = mdf_file.CreateCanDataFrameChannelGroup(vlsd_channel_group, 3, "can0");

    zassert_true(
        Throws<std::runtime_error>([&] {
            mdf_file.GetRecordWriter(channel_group);
        }),
        "the layout is sealed by the header, so there is no writer before it"
    );
    zassert_true(
        Throws<std::runtime_error>([&] {
            mdf_file.GetCanRecordWriter(can_group);
        }),
        "the layout is sealed by the header, so there is no CAN writer before it"
    );

    auto first = MakeStream();
    const auto header_bytes_written = mdf_file.WriteHeaderToStream(first);

    auto& writer = mdf_file.GetRecordWriter(channel_group);
    auto& can_writer = mdf_file.GetCanRecordWriter(can_group);
    zassert_equal(&mdf_file.GetRecordWriter(channel_group), &writer, "a group has one writer per header");

    zassert_true(
        Throws<std::runtime_error>([&] {
            mdf_file.GetRecordWriter(can_group);
        }),
        "a CAN group has no value record writer"
    );
    zassert_true(
        Throws<std::runtime_error>([&] {
            mdf_file.GetRecordWriter(vlsd_channel_group);
        }),
        "a VLSD group has no record writer of its own"
    );
    zassert_true(
        Throws<std::runtime_error>([&] {
            mdf_file.GetCanRecordWriter(channel_group);
        }),
        "a value group has no CAN record writer"
    );

    Mdf4File other_file;
    auto foreign_group = other_file.CreateChannelGroup(1, "pressure");
    zassert_true(
        Throws<std::runtime_error>([&] {
            mdf_file.GetRecordWriter(foreign_group);
        }),
        "a group of another file has no writer here"
    );

    std::array<MdfValue, 2> values{ 0.5, 2.5F };
    zassert_equal(writer.Write(first, values), RECORD_ID_SIZE + TIME_SIZE + 4);
    can_writer.Write(first, MakeCanFrame(8), 0.5, 1);
    mdf_file.FinalizeToStream(first);

    const auto file_bytes = first.str();
    const auto data = AsBytes(file_bytes);
    const auto data_block_address = FindDataBlockAddress(data);
    zassert_equal(
        Read<uint64_t>(data, data_block_address + BLOCK_LENGTH_OFFSET),
        BLOCK_HEADER_SIZE + (RECORD_ID_SIZE + TIME_SIZE + 4) + VlsdRecordSize(8) + CAN_RECORD_SIZE,
        "records written straight through the writers are covered by the DT length"
    );
    zassert_equal(
        data.size(), header_bytes_written + (RECORD_ID_SIZE + TIME_SIZE + 4) + VlsdRecordSize(8) + CAN_RECORD_SIZE
    );

    // A new header starts every writer over.
    auto second = MakeStream();
    mdf_file.WriteHeaderToStream(second);
    zassert_equal(mdf_file.GetCanRecordWriter(can_group).GetVlsdOffset(), 0, "a new file restarts the VLSD offsets");
    zassert_equal(mdf_file.GetRecordWriter(channel_group).GetBytesWritten(), 0, "a new file restarts the byte count");
}

ZTEST(mdf_file, test_TextBlocksAreSharedBetweenChannels) {
    Mdf4File mdf_file;

    const auto first = mdf_file.GetOrCreateTextBlock("bar");
    const auto second = mdf_file.GetOrCreateTextBlock("bar");
    const auto other = mdf_file.GetOrCreateTextBlock("psi");

    zassert_equal(first, second, "identical texts must share one TX block");
    zassert_not_equal(first, other);
}

ZTEST(mdf_file, test_CanDlcFollowsTheIso11898Table) {
    const struct {
        size_t payload_size;
        uint8_t expected_dlc;
    } cases[] = { { 0, 0 }, { 1, 1 }, { 8, 8 }, { 12, 9 }, { 16, 10 }, { 20, 11 }, { 24, 12 }, { 32, 13 }, { 48, 14 },
        { 64, 15 } };

    for(const auto& test_case : cases) {
        Mdf4File mdf_file;
        const auto vlsd_channel_group = mdf_file.CreateVLSDChannelGroup(1);
        const auto can_group = mdf_file.CreateCanDataFrameChannelGroup(vlsd_channel_group, 2, "Raw CAN Frame");

        auto file = MakeStream();
        const auto header_bytes_written = mdf_file.WriteHeaderToStream(file);

        mdf_file.WriteCanbusDataRecordToStream(can_group, file, MakeCanFrame(test_case.payload_size), 0.0, 1);

        const auto file_bytes = file.str();
        const auto data = AsBytes(file_bytes);
        const auto record = header_bytes_written + VlsdRecordSize(test_case.payload_size) + RECORD_ID_SIZE;

        zassert_equal(
            data[record + CAN_DATA_LENGTH], test_case.payload_size, "data length for a %zu byte payload",
            test_case.payload_size
        );
        zassert_equal(
            data[record + CAN_FLAGS] & 0xFU, test_case.expected_dlc, "DLC for a %zu byte payload",
            test_case.payload_size
        );
    }
}

ZTEST(mdf_file, test_RejectsAnOversizedCanPayload) {
    Mdf4File mdf_file;
    const auto vlsd_channel_group = mdf_file.CreateVLSDChannelGroup(1);
    const auto can_group = mdf_file.CreateCanDataFrameChannelGroup(vlsd_channel_group, 2, "Raw CAN Frame");

    auto file = MakeStream();
    const auto header_bytes_written = mdf_file.WriteHeaderToStream(file);

    // CanFramePayload caps itself at the CAN FD limit, so a record can never
    // reach the writer with a length that has no DLC.
    auto can_frame = MakeCanFrame(65);
    zassert_equal(can_frame.data.size(), 64);

    mdf_file.WriteCanbusDataRecordToStream(can_group, file, can_frame, 0.0, 1);

    const auto file_bytes = file.str();
    const auto data = AsBytes(file_bytes);
    const auto record = header_bytes_written + VlsdRecordSize(64) + RECORD_ID_SIZE;

    zassert_equal(data[record + CAN_DATA_LENGTH], 64);
    zassert_equal(data[record + CAN_FLAGS] & 0xFU, 15, "64 bytes must encode as DLC 15");
    zassert_equal(can_group->GetCycleCount(), 1);
}

ZTEST(mdf_file, test_ExtendedAndStandardIdsAreMaskedToTheirWidth) {
    Mdf4File mdf_file;
    const auto vlsd_channel_group = mdf_file.CreateVLSDChannelGroup(1);
    const auto can_group = mdf_file.CreateCanDataFrameChannelGroup(vlsd_channel_group, 2, "Raw CAN Frame");

    auto file = MakeStream();
    const auto header_bytes_written = mdf_file.WriteHeaderToStream(file);

    // Values wider than the field must not spill into the neighbouring IDE bit.
    auto standard = MakeCanFrame(1);
    standard.id = 0xFFFFFFFF;
    standard.is_extended = false;
    mdf_file.WriteCanbusDataRecordToStream(can_group, file, standard, 0.0, 1);

    auto extended = MakeCanFrame(1);
    extended.id = 0xFFFFFFFF;
    extended.is_extended = true;
    mdf_file.WriteCanbusDataRecordToStream(can_group, file, extended, 0.0, 1);

    const auto file_bytes = file.str();
    const auto data = AsBytes(file_bytes);
    const auto record_size = RECORD_ID_SIZE + can_group->GetDataSizeBytes();
    const auto vlsd_record_size = VlsdRecordSize(1);
    zassert_equal(record_size, CAN_RECORD_SIZE);

    const auto standard_pack = Read<uint32_t>(data, header_bytes_written + vlsd_record_size + RECORD_ID_SIZE + CAN_ID);
    zassert_equal(standard_pack & 0x1FFFFFFFU, 0x7FFU, "an 11 bit id is masked");
    zassert_equal(standard_pack >> 29, 0, "IDE and the reserved bits stay clear");

    const auto extended_offset = header_bytes_written + 2 * vlsd_record_size + record_size + RECORD_ID_SIZE + CAN_ID;
    const auto extended_pack = Read<uint32_t>(data, extended_offset);
    zassert_equal(extended_pack & 0x1FFFFFFFU, 0x1FFFFFFFU, "a 29 bit id is masked");
    zassert_equal((extended_pack >> 29) & 0x3U, 0, "the reserved bits stay clear");
    zassert_equal(extended_pack >> 31, 1, "IDE is set");
}

ZTEST(mdf_file, test_HeaderCanBeRewrittenForANewFile) {
    Mdf4File mdf_file;

    auto channel_group = mdf_file.CreateChannelGroup(1, "pressure");
    mdf_file.CreateDataChannel(channel_group, MdfDataType::Float32, "value", "bar");

    const auto vlsd_channel_group = mdf_file.CreateVLSDChannelGroup(2);
    auto can_group = mdf_file.CreateCanDataFrameChannelGroup(vlsd_channel_group, 3, "Raw CAN Frame");

    const auto write_file = [&](MemoryStreamBuf& stream, int record_count) {
        mdf_file.WriteHeaderToStream(stream);

        for(int i = 0; i < record_count; i++) {
            std::array<MdfValue, 2> values{ static_cast<double>(i), static_cast<float>(i) };
            mdf_file.WriteDataRecordToStream(channel_group, stream, values);
            mdf_file.WriteCanbusDataRecordToStream(can_group, stream, MakeCanFrame(8), static_cast<double>(i), 1);
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
    Mdf4File mdf_file;
    mdf_file.CreateChannelGroup(1, "pressure");

    auto file = MakeStream();
    zassert_true(Throws<std::runtime_error>([&] {
        mdf_file.FinalizeToStream(file);
    }));
}

ZTEST(mdf_file, test_FinalizeNeedsASeekableStream) {
    Mdf4File mdf_file;
    const auto channel_group = mdf_file.CreateChannelGroup(1, "pressure");

    NonSeekableStreamBuf stream;
    mdf_file.WriteHeaderToStream(stream);

    std::array<MdfValue, 1> values{ 0.0 };
    mdf_file.WriteDataRecordToStream(channel_group, stream, values);

    bool threw = false;
    try {
        mdf_file.FinalizeToStream(stream);
    } catch(const std::ios_base::failure&) {
        threw = true;
    }
    zassert_true(threw, "a stream that cannot seek leaves the file unfinalized instead of corrupt");
}

// On FAT the file size and the data reach the card at sync; the identifier flip must not get
// there before the lengths and counts it vouches for.
ZTEST(mdf_file, test_FinalizeSyncsBeforeAndAfterTheIdentifier) {
    Mdf4File mdf_file;
    const auto channel_group = mdf_file.CreateChannelGroup(1, "pressure");
    mdf_file.CreateDataChannel(channel_group, MdfDataType::Float32, "value", "bar");

    SyncProbeStreamBuf stream;
    const auto header_bytes_written = mdf_file.WriteHeaderToStream(stream);
    zassert_true(stream.Snapshots().empty(), "writing the header must not sync");

    const auto header_bytes = stream.str();
    const auto data_block_address = FindDataBlockAddress(AsBytes(header_bytes));
    zassert_equal(
        data_block_address + BLOCK_HEADER_SIZE, header_bytes_written, "DT block not found at the end of the header"
    );
    stream.SetDataBlockAddress(data_block_address);

    const auto channel_group_address = static_cast<size_t>(channel_group->GetAddress());
    zassert_equal(
        BlockId(AsBytes(header_bytes), channel_group_address), std::string("##CG"),
        "the channel group's address must point at its block in the header"
    );
    stream.SetChannelGroupAddress(channel_group_address);

    constexpr uint64_t record_count = 4;
    for(uint64_t i = 0; i < record_count; i++) {
        std::array<MdfValue, 2> values{ static_cast<double>(i), static_cast<float>(i) };
        mdf_file.WriteDataRecordToStream(channel_group, stream, values);
    }

    mdf_file.FinalizeToStream(stream);

    const auto& snapshots = stream.Snapshots();
    zassert_equal(snapshots.size(), 2, "finalize must sync exactly twice: before and after the identifier");

    const uint64_t expected_data_block_length = BLOCK_HEADER_SIZE
        + record_count * (RECORD_ID_SIZE + channel_group->GetDataSizeBytes());

    zassert_equal(
        snapshots[0].identifier, std::string("UnFinMF "), "at the first sync the file must still be marked unfinalized"
    );
    zassert_equal(
        snapshots[0].data_block_length, expected_data_block_length,
        "at the first sync the DT length must already cover the records"
    );
    zassert_equal(
        snapshots[0].cycle_count, record_count, "at the first sync the CG cycle count must already be patched"
    );

    zassert_equal(snapshots[1].identifier, std::string("MDF     "), "at the second sync the identifier must be final");
    zassert_equal(
        snapshots[1].data_block_length, expected_data_block_length, "the DT length must not change between the syncs"
    );
    zassert_equal(snapshots[1].cycle_count, record_count, "the CG cycle count must not change between the syncs");
}

ZTEST(mdf_file, test_FinalizeThrowsWhenTheSyncFails) {
    Mdf4File mdf_file;
    const auto channel_group = mdf_file.CreateChannelGroup(1, "pressure");
    mdf_file.CreateDataChannel(channel_group, MdfDataType::Float32, "value", "bar");

    SyncProbeStreamBuf stream;
    mdf_file.WriteHeaderToStream(stream);

    std::array<MdfValue, 2> values{ 0.0, 0.0F };
    mdf_file.WriteDataRecordToStream(channel_group, stream, values);

    stream.FailSync();
    zassert_true(
        Throws<std::ios_base::failure>([&] {
            mdf_file.FinalizeToStream(stream);
        }),
        "a sync that fails must surface as an I/O failure, not as a finalized file"
    );

    const auto file_bytes = stream.str();
    zassert_mem_equal(
        file_bytes.data(), "UnFinMF ", 8, "the identifier must not flip when the lengths could not be synced first"
    );
}

ZTEST(mdf_file, test_DataGroupIsSharedByEveryChannelGroup) {
    Mdf4File mdf_file({ .record_id_size_bytes = RECORD_ID_SIZE });

    const auto data_group = mdf_file.GetDataGroup();
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

ZTEST(mdf_file, test_Float64MasterChannelLayout) {
    Mdf4File mdf_file;
    const auto channel_group = mdf_file.CreateChannelGroup(1, "pressure");
    const auto value_channel = mdf_file.CreateDataChannel(channel_group, MdfDataType::Float32, "value", "bar");

    const auto master = channel_group->GetChannels().at(0);
    zassert_equal(master->GetType(), mdf4::ChannelBlock::Type::Master);
    zassert_equal(master->GetDataType(), mdf4::ChannelBlock::DataType::FloatLe);
    zassert_equal(master->GetBitCount(), 64, "the default master time channel is a Float64");
    zassert_equal(master->GetDataOffsetBytes(), 0);
    zassert_equal(value_channel->GetDataOffsetBytes(), TIME_SIZE, "data channels follow the 8 byte time");
    zassert_equal(channel_group->GetDataSizeBytes(), TIME_SIZE + 4);

    auto file = MakeStream();
    const auto header_bytes_written = mdf_file.WriteHeaderToStream(file);

    std::array<MdfValue, 2> values{ 1.5, 2.5F };
    zassert_equal(mdf_file.WriteDataRecordToStream(channel_group, file, values), RECORD_ID_SIZE + TIME_SIZE + 4);

    const auto file_bytes = file.str();
    const auto data = AsBytes(file_bytes);
    const auto record = header_bytes_written + RECORD_ID_SIZE;

    zassert_equal(data.size(), header_bytes_written + RECORD_ID_SIZE + TIME_SIZE + 4);
    zassert_equal(Read<double>(data, record), 1.5, "the time is written as a double");
    zassert_equal(Read<float>(data, record + TIME_SIZE), 2.5F);

    const auto fields = ReadChannelFields(data, static_cast<size_t>(master->GetAddress()));
    zassert_equal(fields.type, std::to_underlying(mdf4::ChannelBlock::Type::Master));
    zassert_equal(fields.sync_type, std::to_underlying(mdf4::ChannelBlock::SyncType::Time));
    zassert_equal(fields.data_type, std::to_underlying(mdf4::ChannelBlock::DataType::FloatLe));
    zassert_equal(fields.bit_count, 64, "cn_bit_count of the master channel");
    zassert_equal(fields.byte_offset, 0);
}

ZTEST(mdf_file, test_Float32TimeOptionKeepsTheOldWidth) {
    Mdf4File mdf_file({ .time_type = MdfDataType::Float32 });
    auto channel_group = mdf_file.CreateChannelGroup(1, "pressure");
    mdf_file.CreateDataChannel(channel_group, MdfDataType::Float32, "value", "bar");

    const auto vlsd_channel_group = mdf_file.CreateVLSDChannelGroup(2);
    const auto can_group = mdf_file.CreateCanDataFrameChannelGroup(vlsd_channel_group, 3, "Raw CAN Frame");

    zassert_equal(channel_group->GetChannels().at(0)->GetBitCount(), 32, "a Float32 master channel stays 4 bytes wide");
    zassert_equal(channel_group->GetDataSizeBytes(), 4 + 4);
    zassert_equal(can_group->GetDataSizeBytes(), 4 + CAN_FRAME_SIZE, "the CAN members shift with the time width");

    auto file = MakeStream();
    const auto header_bytes_written = mdf_file.WriteHeaderToStream(file);

    std::array<MdfValue, 2> values{ 1.5F, 2.5F };
    mdf_file.WriteDataRecordToStream(channel_group, file, values);

    std::array<MdfValue, 2> double_time{ 1.5, 2.5F };
    zassert_true(
        Throws<std::runtime_error>([&] {
            mdf_file.WriteDataRecordToStream(channel_group, file, double_time);
        }),
        "a Float32 master channel must refuse a double time"
    );

    mdf_file.WriteCanbusDataRecordToStream(can_group, file, MakeCanFrame(8), 1.5, 1);

    const auto file_bytes = file.str();
    const auto data = AsBytes(file_bytes);

    const auto value_record = header_bytes_written + RECORD_ID_SIZE;
    zassert_equal(Read<float>(data, value_record), 1.5F);
    zassert_equal(Read<float>(data, value_record + 4), 2.5F);

    const auto can_record = header_bytes_written + RECORD_ID_SIZE + 4 + 4 + VlsdRecordSize(8) + RECORD_ID_SIZE;
    zassert_equal(Read<float>(data, can_record), 1.5F, "the CAN record time is written as a float");
    zassert_equal(data[can_record + 4], 1, "BusChannel follows the 4 byte time");
    zassert_equal(Read<uint32_t>(data, can_record + 5) & 0x1FFFFFFFU, 0x100U, "ID follows BusChannel");
}

ZTEST(mdf_file, test_VlsdOffsetWidthFollowsTheOption) {
    for(const uint8_t width : { 4, 8 }) {
        Mdf4File mdf_file({ .vlsd_offset_size_bytes = width });
        const auto vlsd_channel_group = mdf_file.CreateVLSDChannelGroup(1);
        const auto can_group = mdf_file.CreateCanDataFrameChannelGroup(vlsd_channel_group, 2, "Raw CAN Frame");

        const auto members = CanFrameMembers(can_group);
        zassert_equal(members.size(), 10, "width %u: every CAN_DataFrame member is linked", width);

        const auto& data_bytes = members.back();
        zassert_equal(data_bytes->GetType(), mdf4::ChannelBlock::Type::VariableLength);
        zassert_equal(data_bytes->GetBitCount(), width * 8U, "width %u: DataBytes carries the offset", width);
        zassert_equal(data_bytes->GetDataOffsetBytes(), CAN_DATA_BYTES);

        const auto frame_size = 7 + width;
        zassert_equal(
            can_group->GetChannels().at(1)->GetBitCount(), frame_size * 8U, "width %u: parent bit count", width
        );
        zassert_equal(can_group->GetDataSizeBytes(), TIME_SIZE + frame_size, "width %u: record data size", width);

        auto file = MakeStream();
        const auto header_bytes_written = mdf_file.WriteHeaderToStream(file);

        mdf_file.WriteCanbusDataRecordToStream(can_group, file, MakeCanFrame(8), 0.0, 1);
        mdf_file.WriteCanbusDataRecordToStream(can_group, file, MakeCanFrame(8), 0.1, 1);

        const auto file_bytes = file.str();
        const auto data = AsBytes(file_bytes);

        const size_t record_size = RECORD_ID_SIZE + TIME_SIZE + frame_size;
        const size_t vlsd_record_size = VlsdRecordSize(8);
        zassert_equal(data.size(), header_bytes_written + 2 * (record_size + vlsd_record_size));

        const auto first_offset = header_bytes_written + vlsd_record_size + RECORD_ID_SIZE + CAN_DATA_BYTES;
        const auto second_offset = first_offset + record_size + vlsd_record_size;

        if(width == 4) {
            zassert_equal(Read<uint32_t>(data, first_offset), 0, "4 byte offset of the first frame");
            zassert_equal(
                Read<uint32_t>(data, second_offset), VLSD_LENGTH_SIZE + 8, "4 byte offset of the second frame"
            );
        } else {
            zassert_equal(Read<uint64_t>(data, first_offset), 0, "8 byte offset of the first frame");
            zassert_equal(
                Read<uint64_t>(data, second_offset), VLSD_LENGTH_SIZE + 8, "8 byte offset of the second frame"
            );
        }
    }
}

ZTEST(mdf_file, test_CanDataFrameMemberOffsetsAndBitFields) {
    Mdf4File mdf_file;
    const auto vlsd_channel_group = mdf_file.CreateVLSDChannelGroup(1);
    const auto can_group = mdf_file.CreateCanDataFrameChannelGroup(vlsd_channel_group, 2, "Raw CAN Frame");

    auto file = MakeStream();
    const auto header_bytes_written = mdf_file.WriteHeaderToStream(file);

    CanFrame can_frame{ .id = 0x1ABCDEF0,
        .is_extended = true,
        .is_transmit = true,
        .is_can_fd = true,
        .is_bitrate_switch = true,
        .data = std::vector<uint8_t>(12, 0x5A) };
    mdf_file.WriteCanbusDataRecordToStream(can_group, file, can_frame, 0.25, 3);

    const auto file_bytes = file.str();
    const auto data = AsBytes(file_bytes);

    const auto parent = can_group->GetChannels().at(1);
    const auto parent_fields = ReadChannelFields(data, static_cast<size_t>(parent->GetAddress()));
    zassert_equal(parent_fields.type, std::to_underlying(mdf4::ChannelBlock::Type::FixedLength));
    zassert_equal(parent_fields.data_type, std::to_underlying(mdf4::ChannelBlock::DataType::ByteArray));
    zassert_equal(parent_fields.byte_offset, TIME_SIZE, "CAN_DataFrame starts after the time");
    zassert_equal(parent_fields.bit_offset, 0);
    zassert_equal(parent_fields.bit_count, CAN_FRAME_SIZE * 8, "CAN_DataFrame spans every member");
    zassert_equal(parent_fields.flags, std::to_underlying(mdf4::ChannelBlock::Flag::BusEvent));
    zassert_equal(
        ReadBlockText(data, ReadLink(data, static_cast<size_t>(parent->GetAddress()), CN_NAME_LINK)),
        std::string("CAN_DataFrame")
    );

    const struct {
        const char* name;
        uint32_t byte_offset;
        uint8_t bit_offset;
        uint32_t bit_count;
        mdf4::ChannelBlock::DataType data_type;
        mdf4::ChannelBlock::Type type;
    } expected[] = {
        { "CAN_DataFrame.BusChannel", CAN_BUS_CHANNEL, 0, 8, mdf4::ChannelBlock::DataType::UnsignedIntegerLe,
            mdf4::ChannelBlock::Type::FixedLength },
        { "CAN_DataFrame.ID", CAN_ID, 0, 29, mdf4::ChannelBlock::DataType::UnsignedIntegerLe,
            mdf4::ChannelBlock::Type::FixedLength },
        { "CAN_DataFrame.IDE", CAN_ID + 3, 7, 1, mdf4::ChannelBlock::DataType::UnsignedIntegerLe,
            mdf4::ChannelBlock::Type::FixedLength },
        { "CAN_DataFrame.DLC", CAN_FLAGS, 0, 4, mdf4::ChannelBlock::DataType::UnsignedIntegerLe,
            mdf4::ChannelBlock::Type::FixedLength },
        { "CAN_DataFrame.Dir", CAN_FLAGS, 4, 1, mdf4::ChannelBlock::DataType::UnsignedIntegerLe,
            mdf4::ChannelBlock::Type::FixedLength },
        { "CAN_DataFrame.EDL", CAN_FLAGS, 5, 1, mdf4::ChannelBlock::DataType::UnsignedIntegerLe,
            mdf4::ChannelBlock::Type::FixedLength },
        { "CAN_DataFrame.BRS", CAN_FLAGS, 6, 1, mdf4::ChannelBlock::DataType::UnsignedIntegerLe,
            mdf4::ChannelBlock::Type::FixedLength },
        { "CAN_DataFrame.ESI", CAN_FLAGS, 7, 1, mdf4::ChannelBlock::DataType::UnsignedIntegerLe,
            mdf4::ChannelBlock::Type::FixedLength },
        { "CAN_DataFrame.DataLength", CAN_DATA_LENGTH, 0, 8, mdf4::ChannelBlock::DataType::UnsignedIntegerLe,
            mdf4::ChannelBlock::Type::FixedLength },
        { "CAN_DataFrame.DataBytes", CAN_DATA_BYTES, 0, 64, mdf4::ChannelBlock::DataType::ByteArray,
            mdf4::ChannelBlock::Type::VariableLength },
    };

    const auto members = CanFrameMembers(can_group);
    zassert_equal(members.size(), std::size(expected), "every member must be linked exactly once");

    for(size_t i = 0; i < std::size(expected); i++) {
        const auto address = static_cast<size_t>(members[i]->GetAddress());
        const auto fields = ReadChannelFields(data, address);
        const auto name = ReadBlockText(data, ReadLink(data, address, CN_NAME_LINK));

        zassert_equal(
            name, std::string(expected[i].name), "member %zu is %s, expected %s", i, name.c_str(), expected[i].name
        );
        zassert_equal(fields.byte_offset, expected[i].byte_offset, "%s cn_byte_offset", expected[i].name);
        zassert_equal(fields.bit_offset, expected[i].bit_offset, "%s cn_bit_offset", expected[i].name);
        zassert_equal(fields.bit_count, expected[i].bit_count, "%s cn_bit_count", expected[i].name);
        zassert_equal(fields.data_type, std::to_underlying(expected[i].data_type), "%s cn_data_type", expected[i].name);
        zassert_equal(fields.type, std::to_underlying(expected[i].type), "%s cn_type", expected[i].name);
        zassert_equal(
            fields.flags, std::to_underlying(mdf4::ChannelBlock::Flag::BusEvent), "%s cn_flags", expected[i].name
        );
    }

    const auto record = header_bytes_written + VlsdRecordSize(12) + RECORD_ID_SIZE;
    zassert_equal(Read<double>(data, record), 0.25);
    zassert_equal(data[record + CAN_BUS_CHANNEL], 3, "BusChannel is written as given");

    const auto id_pack = Read<uint32_t>(data, record + CAN_ID);
    zassert_equal(id_pack & 0x1FFFFFFFU, 0x1ABCDEF0U, "ID occupies the low 29 bits");
    zassert_equal(id_pack >> 31, 1, "IDE is bit 31 of the ID word");

    const auto flags = static_cast<uint8_t>(data[record + CAN_FLAGS]);
    zassert_equal(flags & 0xFU, 9, "DLC in bits 0-3");
    zassert_equal((flags >> 4) & 0x1U, 1, "Dir in bit 4");
    zassert_equal((flags >> 5) & 0x1U, 1, "EDL in bit 5");
    zassert_equal((flags >> 6) & 0x1U, 1, "BRS in bit 6");
    zassert_equal((flags >> 7) & 0x1U, 0, "ESI in bit 7 is always zero: CanFrame has no ESI field");

    zassert_equal(data[record + CAN_DATA_LENGTH], 12, "DataLength is a whole byte");
    zassert_equal(Read<uint64_t>(data, record + CAN_DATA_BYTES), 0, "the first VLSD offset is 0");
}

ZTEST(mdf_file, test_HeaderCommentCarriesCommonProperties) {
    Mdf4File bare_file;
    bare_file.CreateChannelGroup(1, "pressure");
    auto bare_stream = MakeStream();
    bare_file.WriteHeaderToStream(bare_stream);

    const auto bare_bytes = bare_stream.str();
    zassert_equal(
        ReadLink(AsBytes(bare_bytes), ID_BLOCK_SIZE, HD_COMMENT_LINK), 0, "without a comment hd_md_comment stays NIL"
    );

    Mdf4File mdf_file;
    const std::array<MdfProperty, 2> properties{ { { "device_name", "bench-01" }, { "product_id", "42" } } };
    mdf_file.SetHeaderComment("rt_daq bench", properties);
    mdf_file.CreateChannelGroup(1, "pressure");

    auto file = MakeStream();
    mdf_file.WriteHeaderToStream(file);

    const auto file_bytes = file.str();
    const auto data = AsBytes(file_bytes);

    zassert_equal(BlockId(data, ID_BLOCK_SIZE), std::string("##HD"));
    const auto comment_address = ReadLink(data, ID_BLOCK_SIZE, HD_COMMENT_LINK);
    zassert_not_equal(comment_address, 0, "hd_md_comment must point at the comment block");
    zassert_equal(BlockId(data, comment_address), std::string("##MD"));

    const auto text = ReadBlockText(data, comment_address);
    zassert_equal(
        text,
        std::string(
            "<HDcomment><TX>rt_daq bench</TX>"
            "<common_properties><e name=\"device_name\">bench-01</e><e name=\"product_id\">42</e></common_properties>"
            "</HDcomment>"
        ),
        "%s", text.c_str()
    );
}

ZTEST(mdf_file, test_FileHistoryToolFields) {
    Mdf4File mdf_file;
    mdf_file.SetFileHistory("rt_daq", "Eerie & Leap", "1.2.3");
    mdf_file.CreateChannelGroup(1, "pressure");

    auto file = MakeStream();
    mdf_file.WriteHeaderToStream(file);

    const auto file_bytes = file.str();
    const auto data = AsBytes(file_bytes);

    const auto file_history_address = ReadLink(data, ID_BLOCK_SIZE, HD_FILE_HISTORY_LINK);
    zassert_equal(
        BlockId(data, file_history_address), std::string("##FH"), "hd_fh_first must point at the file history"
    );

    const auto comment_address = ReadLink(data, file_history_address, FH_COMMENT_LINK);
    zassert_equal(BlockId(data, comment_address), std::string("##MD"), "fh_md_comment must point at the comment block");

    const auto text = ReadBlockText(data, comment_address);
    zassert_equal(
        text,
        std::string(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?><FHcomment><TX>created</TX>"
            "<tool_id>rt_daq</tool_id><tool_vendor>Eerie &amp; Leap</tool_vendor><tool_version>1.2.3</tool_version>"
            "</FHcomment>"
        ),
        "%s", text.c_str()
    );
}

ZTEST(mdf_file, test_ChannelAndGroupCommentsAreLinked) {
    Mdf4File mdf_file;
    const auto channel_group = mdf_file.CreateChannelGroup(1, "pressure");
    const auto channel = mdf_file.CreateDataChannel(channel_group, MdfDataType::Float32, "value", "bar");

    mdf_file.SetChannelGroupComment(channel_group, "Oil pressure <sensor>");
    mdf_file.SetChannelComment(channel, "bar, 0..10 & more");

    auto file = MakeStream();
    mdf_file.WriteHeaderToStream(file);

    const auto file_bytes = file.str();
    const auto data = AsBytes(file_bytes);

    const auto group_address = static_cast<size_t>(channel_group->GetAddress());
    zassert_equal(BlockId(data, group_address), std::string("##CG"));
    const auto group_comment_address = ReadLink(data, group_address, CG_COMMENT_LINK);
    zassert_equal(
        BlockId(data, group_comment_address), std::string("##MD"), "cg_md_comment must point at the comment block"
    );
    zassert_equal(
        ReadBlockText(data, group_comment_address),
        std::string("<CGcomment><TX>Oil pressure &lt;sensor&gt;</TX></CGcomment>")
    );

    const auto channel_address = static_cast<size_t>(channel->GetAddress());
    const auto channel_comment_address = ReadLink(data, channel_address, CN_COMMENT_LINK);
    zassert_equal(
        BlockId(data, channel_comment_address), std::string("##MD"), "cn_md_comment must point at the comment block"
    );
    zassert_equal(
        ReadBlockText(data, channel_comment_address),
        std::string("<CNcomment><TX>bar, 0..10 &amp; more</TX></CNcomment>")
    );

    const auto master_address = static_cast<size_t>(channel_group->GetChannels().at(0)->GetAddress());
    zassert_equal(ReadLink(data, master_address, CN_COMMENT_LINK), 0, "a channel without a comment keeps a NIL link");
}

ZTEST(mdf_file, test_MetadataSettersAreRefusedAfterTheHeader) {
    Mdf4File mdf_file;
    auto channel_group = mdf_file.CreateChannelGroup(1, "pressure");
    auto channel = mdf_file.CreateDataChannel(channel_group, MdfDataType::Float32, "value", "bar");

    const std::array<MdfProperty, 1> properties{ { { "device_name", "bench-01" } } };

    // Before the header every setter is accepted.
    mdf_file.SetHeaderComment("early", properties);
    mdf_file.SetFileHistory("rt_daq", "Eerie Leap", "1.0.0");
    mdf_file.SetChannelComment(channel, "early");
    mdf_file.SetChannelGroupComment(channel_group, "early");

    auto file = MakeStream();
    mdf_file.WriteHeaderToStream(file);

    // A comment block added now would never be written and its link would dangle.
    zassert_true(
        Throws<std::runtime_error>([&] {
            mdf_file.SetHeaderComment("late", properties);
        }),
        "the header comment cannot change once the header is on the medium"
    );
    zassert_true(
        Throws<std::runtime_error>([&] {
            mdf_file.SetFileHistory("late", "late", "9.9.9");
        }),
        "the file history cannot change once the header is on the medium"
    );
    zassert_true(
        Throws<std::runtime_error>([&] {
            mdf_file.SetChannelComment(channel, "late");
        }),
        "a channel comment cannot change once the header is on the medium"
    );
    zassert_true(
        Throws<std::runtime_error>([&] {
            mdf_file.SetChannelGroupComment(channel_group, "late");
        }),
        "a channel group comment cannot change once the header is on the medium"
    );
}

#ifdef CONFIG_ARCH_POSIX
// Writes a complete file to the host file system for an asammdf round trip; enabled per run with
// CONFIG_MDF_TEST_DUMP_DIR (native_sim links the host libc, so std::ofstream reaches the host).
ZTEST(mdf_file, test_DumpFileForAsammdf) {
    const std::string_view dump_dir = CONFIG_MDF_TEST_DUMP_DIR;
    if(dump_dir.empty())
        ztest_test_skip();

    RtcProvider rtc_time_provider;

    Mdf4File mdf_file;
    mdf_file.UpdateCurrentTime(rtc_time_provider.GetTime());
    mdf_file.SetFileHistory("rt_core unit test", "Eerie Leap", "0.0.0");

    const std::array<MdfProperty, 3> properties{ { { "device_name", "unit <test> & dump" }, { "product_id", "42" },
        { "sensors_configuration_hash", "3735928559" } } };
    mdf_file.SetHeaderComment("rt_core mdf unit dump", properties);

    const auto source_information = std::make_shared<mdf4::SourceInformationBlock>(
        mdf4::SourceInformationBlock::SourceType::IoDevice, mdf4::SourceInformationBlock::BusType::None
    );
    source_information->SetName(mdf_file.GetOrCreateTextBlock("Eerie Leap Sensor"));

    const auto pressure_group = mdf_file.CreateChannelGroup(1, "oil_pressure");
    pressure_group->AddSourceInformation(source_information);
    mdf_file.SetChannelGroupComment(pressure_group, "Oil pressure, \"main gallery\"");
    const auto pressure_channel = mdf_file.CreateDataChannel(
        pressure_group, MdfDataType::Uint64, "oil_pressure", "bar"
    );
    pressure_channel->SetConversion(mdf_file.CreateAlgebraicConversion("X * 0.1"));
    mdf_file.SetChannelComment(pressure_channel, "raw counts <0..1000> & scaled by 0.1");

    const auto temperature_group = mdf_file.CreateChannelGroup(2, "coolant_temperature");
    temperature_group->AddSourceInformation(source_information);
    const auto temperature_channel = mdf_file.CreateDataChannel(
        temperature_group, MdfDataType::Float32, "coolant_temperature", "C"
    );
    mdf_file.SetChannelComment(temperature_channel, "Coolant temperature at the outlet");

    const auto vlsd_channel_group = mdf_file.CreateVLSDChannelGroup(3);
    const auto can_group = mdf_file.CreateCanDataFrameChannelGroup(vlsd_channel_group, 4, "can0");

    const std::string path = std::string(dump_dir) + "/mdf_unit_dump.mf4";
    std::ofstream file_stream(path, std::ios::binary | std::ios::trunc);
    zassert_true(file_stream.is_open(), "cannot create %s", path.c_str());
    auto& file = *file_stream.rdbuf();

    mdf_file.WriteHeaderToStream(file);

    for(uint64_t i = 0; i < 10; i++) {
        std::array<MdfValue, 2> pressure{ static_cast<double>(i) * 0.1, 100 + i * 10 };
        mdf_file.WriteDataRecordToStream(pressure_group, file, pressure);

        std::array<MdfValue, 2> temperature{ static_cast<double>(i) * 0.1 + 0.000001,
            80.0F + static_cast<float>(i) * 0.5F };
        mdf_file.WriteDataRecordToStream(temperature_group, file, temperature);
    }

    CanFrame classic{ .id = 0x100,
        .is_extended = false,
        .is_transmit = false,
        .is_can_fd = false,
        .is_bitrate_switch = false,
        .data = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 } };
    mdf_file.WriteCanbusDataRecordToStream(can_group, file, classic, 0.05, 1);

    CanFrame fd_12{ .id = 0x18FF1234,
        .is_extended = true,
        .is_transmit = false,
        .is_can_fd = true,
        .is_bitrate_switch = true,
        .data = std::vector<uint8_t>(12, 0xAB) };
    mdf_file.WriteCanbusDataRecordToStream(can_group, file, fd_12, 0.15, 1);

    CanFrame fd_64 = MakeCanFrame(64);
    fd_64.id = 0x7FF;
    fd_64.is_transmit = true;
    mdf_file.WriteCanbusDataRecordToStream(can_group, file, fd_64, 0.25, 1);

    mdf_file.FinalizeToStream(file);
    file_stream.close();
    zassert_true(file_stream.good(), "writing %s failed", path.c_str());
}
#endif

ZTEST(mdf_file, test_DataGroupOutlivingItsFileHoldsNoFreedGroups) {
    std::shared_ptr<mdf4::DataGroupBlock> data_group;
    {
        Mdf4File mdf_file;
        mdf_file.CreateChannelGroup(1, "first");
        mdf_file.CreateChannelGroup(2, "second");
        data_group = mdf_file.GetDataGroup();
    }

    // The file cut its chain and freed the groups; adding to the data group must start a new chain
    // rather than link from the freed tail.
    zassert_is_null(data_group->GetChildren()[1].get(), "the chain the file owned is gone with it");
    data_group->AddChannelGroup(std::make_shared<mdf4::ChannelGroupBlock>(1, 3));
    zassert_not_null(data_group->GetChildren()[1].get(), "a new group starts a new chain");
}
