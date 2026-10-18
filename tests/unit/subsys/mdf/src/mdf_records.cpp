#include <array>
#include <cstdint>
#include <ios>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <zephyr/ztest.h>

#include "subsys/canbus/can_frame.h"
#include "subsys/mdf/mdf4/can_record_writer.h"
#include "subsys/mdf/mdf4/channel_block.h"
#include "subsys/mdf/mdf4/channel_group_block.h"
#include "subsys/mdf/mdf4/record_writer.h"
#include "subsys/mdf/mdf4_file.h"
#include "subsys/mdf/mdf_data_type.h"
#include "subsys/mdf/mdf_value.h"

#include "mdf_test_support.h"

using namespace eerie_leap::subsys::mdf;
using namespace eerie_leap::subsys::mdf::mdf4;
using namespace mdf_test;
using eerie_leap::subsys::canbus::CanFrame;

namespace {

constexpr size_t TIME_SIZE = 8;
constexpr size_t CAN_FRAME_SIZE = 15;
constexpr size_t VLSD_LENGTH_SIZE = 4;

// Member positions inside a CAN record's data, after the record id and the Float64 time.
constexpr size_t CAN_DATA_LENGTH = TIME_SIZE + 6;
constexpr size_t CAN_DATA_BYTES = TIME_SIZE + 7;

std::shared_ptr<ChannelBlock> MakeChannel(ChannelBlock::DataType data_type, uint32_t bit_count) {
    return std::make_shared<ChannelBlock>(
        ChannelBlock::Type::FixedLength, ChannelBlock::SyncType::NoSync, data_type, bit_count
    );
}

std::shared_ptr<ChannelGroupBlock> MakeGroup(uint8_t record_id_size, uint64_t record_id) {
    return std::make_shared<ChannelGroupBlock>(record_id_size, record_id);
}

std::shared_ptr<ChannelGroupBlock> MakeVlsdGroup(uint8_t record_id_size, uint64_t record_id) {
    auto group = MakeGroup(record_id_size, record_id);
    group->SetFlags(std::to_underlying(ChannelGroupBlock::Flag::VlsdChannel));

    return group;
}

// The shape CanRecordWriter checks: a Float64 master time channel, then the CAN_DataFrame parent
// spanning the frame members up to the end of the VLSD offset.
std::shared_ptr<ChannelGroupBlock>
MakeCanGroup(uint8_t record_id_size, uint64_t record_id, uint8_t vlsd_offset_size = 8) {
    auto group = MakeGroup(record_id_size, record_id);
    group->AddChannel(MakeChannel(ChannelBlock::DataType::FloatLe, TIME_SIZE * 8));
    group->AddChannel(
        MakeChannel(ChannelBlock::DataType::ByteArray, CanRecordWriter::FrameSizeBytes(vlsd_offset_size) * 8)
    );

    return group;
}

CanFrame MakeCanFrame(size_t payload_size) {
    CanFrame frame;
    frame.id = 0x100;
    frame.is_can_fd = payload_size > 8;
    frame.data = std::vector<uint8_t>(payload_size, 0x5A);

    return frame;
}

} // namespace

ZTEST_SUITE(mdf_records, NULL, NULL, NULL, NULL, NULL);

ZTEST(mdf_records, test_RecordWriterWritesEveryValueAtItsChannelOffset) {
    const auto group = MakeGroup(2, 0x0102);
    group->AddChannel(MakeChannel(ChannelBlock::DataType::FloatLe, 32));
    group->AddChannel(MakeChannel(ChannelBlock::DataType::UnsignedIntegerLe, 64));
    group->AddChannel(MakeChannel(ChannelBlock::DataType::SignedIntegerLe, 32));

    RecordWriter writer(*group);
    zassert_equal(writer.GetRecordSizeBytes(), 2 + 16, "the record is the id and every channel");

    auto stream = MakeStream();
    std::array<MdfValue, 3> values{ 1.5F, uint64_t{ 0xAABBCCDDEEFF0011ULL }, int32_t{ -2 } };

    zassert_equal(writer.Write(stream, values), 18, "record id bytes are part of the record");

    const auto written = stream.Str();
    const auto data = AsBytes(written);
    zassert_equal(data.size(), 18);
    zassert_equal(Read<uint16_t>(data, 0), 0x0102, "record id prefix");
    zassert_equal(Read<float>(data, 2), 1.5F);
    zassert_equal(Read<uint64_t>(data, 6), 0xAABBCCDDEEFF0011ULL);
    zassert_equal(Read<int32_t>(data, 14), -2);
    zassert_equal(writer.GetBytesWritten(), 18, "the writer counts what it wrote for the DT length");
}

ZTEST(mdf_records, test_RecordWriterSupportsEveryValueType) {
    const auto group = MakeGroup(1, 1);
    group->AddChannel(MakeChannel(ChannelBlock::DataType::SignedIntegerLe, 32));
    group->AddChannel(MakeChannel(ChannelBlock::DataType::SignedIntegerLe, 64));
    group->AddChannel(MakeChannel(ChannelBlock::DataType::UnsignedIntegerLe, 32));
    group->AddChannel(MakeChannel(ChannelBlock::DataType::UnsignedIntegerLe, 64));
    group->AddChannel(MakeChannel(ChannelBlock::DataType::FloatLe, 32));
    group->AddChannel(MakeChannel(ChannelBlock::DataType::FloatLe, 64));

    RecordWriter writer(*group);
    auto stream = MakeStream();

    std::array<MdfValue, 6> values{ int32_t{ -1 }, int64_t{ -2 }, uint32_t{ 3 }, uint64_t{ 4 }, 5.5F, 6.5 };

    writer.Write(stream, values);

    const auto written = stream.Str();
    const auto data = AsBytes(written);
    zassert_equal(data.size(), 1 + 4 + 8 + 4 + 8 + 4 + 8);
    zassert_equal(Read<int32_t>(data, 1), -1);
    zassert_equal(Read<int64_t>(data, 5), -2);
    zassert_equal(Read<uint32_t>(data, 13), 3);
    zassert_equal(Read<uint64_t>(data, 17), 4);
    zassert_equal(Read<float>(data, 25), 5.5F);
    zassert_equal(Read<double>(data, 29), 6.5);
}

ZTEST(mdf_records, test_RecordWriterCountsCycles) {
    const auto group = MakeGroup(1, 1);
    group->AddChannel(MakeChannel(ChannelBlock::DataType::FloatLe, 32));

    RecordWriter writer(*group);
    auto stream = MakeStream();

    std::array<MdfValue, 1> values{ 0.0F };
    for(int i = 0; i < 5; i++)
        writer.Write(stream, values);

    zassert_equal(group->GetCycleCount(), 5, "cg_cycle_count has to follow the records that were written");
    zassert_equal(writer.GetBytesWritten(), 5ULL * (1 + 4));
}

ZTEST(mdf_records, test_RecordWriterIssuesOneWritePerRecord) {
    const auto group = MakeGroup(1, 1);
    group->AddChannel(MakeChannel(ChannelBlock::DataType::FloatLe, 64));
    group->AddChannel(MakeChannel(ChannelBlock::DataType::FloatLe, 32));
    group->AddChannel(MakeChannel(ChannelBlock::DataType::FloatLe, 32));

    RecordWriter writer(*group);
    WriteCountingStreamBuf stream;

    for(int i = 0; i < 3; i++) {
        std::array<MdfValue, 3> values{ static_cast<double>(i), 1.0F, 2.0F };
        writer.Write(stream, values);
    }

    zassert_equal(stream.WriteCount(), 3, "each record must reach the stream in a single sputn()");
    zassert_equal(stream.Str().size(), 3 * (1 + 8 + 4 + 4));
}

ZTEST(mdf_records, test_RecordWriterRejectsWrongValueCountAndType) {
    const auto group = MakeGroup(1, 1);
    group->AddChannel(MakeChannel(ChannelBlock::DataType::FloatLe, 32));
    group->AddChannel(MakeChannel(ChannelBlock::DataType::UnsignedIntegerLe, 64));

    RecordWriter writer(*group);
    auto stream = MakeStream();

    std::array<MdfValue, 1> too_few{ 0.0F };
    zassert_true(
        Throws<std::runtime_error>([&] {
            writer.Write(stream, too_few);
        }),
        "too few values"
    );

    std::array<MdfValue, 3> too_many{ 0.0F, uint64_t{ 0 }, uint64_t{ 0 } };
    zassert_true(
        Throws<std::runtime_error>([&] {
            writer.Write(stream, too_many);
        }),
        "too many values"
    );

    // A 32 bit value in a 64 bit channel would leave half the field from the previous record.
    std::array<MdfValue, 2> narrow{ 0.0F, uint32_t{ 0 } };
    zassert_true(
        Throws<std::runtime_error>([&] {
            writer.Write(stream, narrow);
        }),
        "uint32 into a uint64 channel"
    );

    std::array<MdfValue, 2> signedness{ 0.0F, int64_t{ 0 } };
    zassert_true(
        Throws<std::runtime_error>([&] {
            writer.Write(stream, signedness);
        }),
        "int64 into a uint64 channel"
    );

    std::array<MdfValue, 2> floatness{ 0.0, uint64_t{ 0 } };
    zassert_true(
        Throws<std::runtime_error>([&] {
            writer.Write(stream, floatness);
        }),
        "double into a float channel"
    );

    zassert_equal(stream.Str().size(), 0, "a rejected record must not reach the stream");
    zassert_equal(group->GetCycleCount(), 0, "a rejected record must not be counted");
    zassert_equal(writer.GetBytesWritten(), 0, "a rejected record must not grow the DT length");
}

ZTEST(mdf_records, test_RecordWriterThrowsOnAShortWrite) {
    const auto group = MakeGroup(1, 1);
    group->AddChannel(MakeChannel(ChannelBlock::DataType::FloatLe, 32));

    RecordWriter writer(*group);
    LimitedStreamBuf stream(3);

    std::array<MdfValue, 1> values{ 1.0F };
    zassert_true(
        Throws<std::ios_base::failure>([&] {
            writer.Write(stream, values);
        }),
        "a truncated record must surface as an I/O failure"
    );
    zassert_equal(group->GetCycleCount(), 0, "a record that did not reach the stream is not counted");
}

ZTEST(mdf_records, test_RecordWriterRejectsLayoutsBeyondItsLimits) {
    auto too_many_fields = MakeGroup(1, 1);
    for(size_t i = 0; i < RecordWriter::MAX_FIELDS + 1; i++)
        too_many_fields->AddChannel(MakeChannel(ChannelBlock::DataType::FloatLe, 32));
    zassert_true(
        Throws<std::runtime_error>([&] {
            RecordWriter writer(*too_many_fields);
        }),
        "%zu channels exceed the field table", RecordWriter::MAX_FIELDS + 1
    );

    // 1 + 8 * 8 = 65 bytes, one more than the record buffer.
    auto too_large = MakeGroup(1, 1);
    for(size_t i = 0; i < RecordWriter::MAX_FIELDS; i++)
        too_large->AddChannel(MakeChannel(ChannelBlock::DataType::FloatLe, 64));
    zassert_true(
        Throws<std::runtime_error>([&] {
            RecordWriter writer(*too_large);
        }),
        "a 65 byte record exceeds the %zu byte buffer", RecordWriter::MAX_RECORD_BYTES
    );

    const auto largest = MakeGroup(8, 1);
    for(size_t i = 0; i < 7; i++)
        largest->AddChannel(MakeChannel(ChannelBlock::DataType::FloatLe, 64));
    zassert_equal(
        RecordWriter(*largest).GetRecordSizeBytes(), RecordWriter::MAX_RECORD_BYTES,
        "a record of exactly the limit is accepted"
    );

    auto byte_array = MakeGroup(1, 1);
    byte_array->AddChannel(MakeChannel(ChannelBlock::DataType::ByteArray, 80));
    zassert_true(
        Throws<std::runtime_error>([&] {
            RecordWriter writer(*byte_array);
        }),
        "a byte array has no MdfValue type"
    );

    auto vlsd = MakeVlsdGroup(1, 1);
    zassert_true(
        Throws<std::runtime_error>([&] {
            RecordWriter writer(*vlsd);
        }),
        "a VLSD group stores variable length records"
    );
}

ZTEST(mdf_records, test_RecordWriterWritesTheInvalidationByteAfterTheData) {
    const auto group = MakeGroup(1, 9);
    group->AddChannel(MakeChannel(ChannelBlock::DataType::FloatLe, 64));
    const auto value = MakeChannel(ChannelBlock::DataType::FloatLe, 32);
    value->SetInvalidationBit(0);
    group->AddChannel(value);
    const auto raw = MakeChannel(ChannelBlock::DataType::FloatLe, 32);
    raw->SetInvalidationBit(1);
    group->AddChannel(raw);
    group->AddChannel(MakeChannel(ChannelBlock::DataType::SignedIntegerLe, 32));
    group->SetInvalidationBytes(1);

    RecordWriter writer(*group);
    constexpr size_t RECORD_SIZE = 1 + TIME_SIZE + 3 * 4 + 1;
    zassert_equal(writer.GetRecordSizeBytes(), RECORD_SIZE, "the record is id, data bytes and the invalidation byte");

    WriteCountingStreamBuf stream;
    const std::array<MdfValue, 4> values{ 0.5, 1.5F, 0.0F, int32_t{ -2 } };
    zassert_equal(writer.Write(stream, values, 0b10), RECORD_SIZE, "the invalidation byte is counted");
    zassert_equal(writer.Write(stream, values), RECORD_SIZE);
    zassert_equal(stream.WriteCount(), 2, "the invalidation byte must go out in the record's single sputn()");

    const std::array<uint8_t, RECORD_SIZE> expected{ 0x09, // record id
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE0, 0x3F,    // time 0.5
        0x00, 0x00, 0xC0, 0x3F,                            // value 1.5
        0x00, 0x00, 0x00, 0x00,                            // raw 0, invalid
        0xFE, 0xFF, 0xFF, 0xFF,                            // -2, no invalidation bit
        0x02 };                                            // bit 1: the raw channel

    const auto written = stream.Str();
    const auto data = AsBytes(written);
    zassert_equal(data.size(), 2 * RECORD_SIZE);
    for(size_t i = 0; i < RECORD_SIZE; i++)
        zassert_equal(
            data[i], expected[i], "byte %zu is 0x%02x, expected 0x%02x", i, static_cast<unsigned>(data[i]),
            static_cast<unsigned>(expected[i])
        );
    zassert_equal(data[2 * RECORD_SIZE - 1], 0, "a record without invalid values clears every bit");
    zassert_equal(writer.GetBytesWritten(), 2ULL * RECORD_SIZE);
}

ZTEST(mdf_records, test_RecordWriterRejectsAMaskBitNoChannelUses) {
    const auto group = MakeGroup(1, 1);
    const auto value = MakeChannel(ChannelBlock::DataType::FloatLe, 32);
    value->SetInvalidationBit(0);
    group->AddChannel(value);
    group->SetInvalidationBytes(1);

    RecordWriter writer(*group);
    auto stream = MakeStream();

    std::array<MdfValue, 1> values{ 1.0F };
    zassert_true(
        Throws<std::runtime_error>([&] {
            writer.Write(stream, values, 0b10);
        }),
        "bit 1 belongs to no channel of the group"
    );
    zassert_true(
        Throws<std::runtime_error>([&] {
            writer.Write(stream, values, 1U << 8);
        }),
        "bit 8 lies past the group's one invalidation byte"
    );

    const auto plain_group = MakeGroup(1, 2);
    plain_group->AddChannel(MakeChannel(ChannelBlock::DataType::FloatLe, 32));
    RecordWriter plain_writer(*plain_group);
    zassert_true(
        Throws<std::runtime_error>([&] {
            plain_writer.Write(stream, values, 1);
        }),
        "a group without invalidation bytes has no bit to set"
    );

    zassert_equal(stream.Str().size(), 0, "a rejected record must not reach the stream");
    zassert_equal(group->GetCycleCount() + plain_group->GetCycleCount(), 0, "a rejected record must not be counted");
}

ZTEST(mdf_records, test_RecordWriterInvalidationLimits) {
    // One byte holds the bits of eight channels.
    const auto eight_bits = MakeGroup(1, 1);
    for(uint32_t i = 0; i < 8; i++) {
        const auto channel = MakeChannel(ChannelBlock::DataType::FloatLe, 32);
        channel->SetInvalidationBit(i);
        eight_bits->AddChannel(channel);
    }
    eight_bits->SetInvalidationBytes(1);

    RecordWriter writer(*eight_bits);
    auto stream = MakeStream();
    const std::array<MdfValue, 8> values{ 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F };
    writer.Write(stream, values, 0xFF);
    const auto written = stream.Str();
    zassert_equal(AsBytes(written).back(), 0xFF, "every one of the eight channels can be invalid");

    auto ninth_bit = MakeGroup(1, 2);
    const auto channel = MakeChannel(ChannelBlock::DataType::FloatLe, 32);
    channel->SetInvalidationBit(8);
    ninth_bit->AddChannel(channel);
    ninth_bit->SetInvalidationBytes(1);
    zassert_true(
        Throws<std::runtime_error>([&] {
            RecordWriter ninth_writer(*ninth_bit);
        }),
        "bit 8 lies outside a single invalidation byte"
    );

    auto wide_mask = MakeGroup(1, 3);
    wide_mask->AddChannel(MakeChannel(ChannelBlock::DataType::FloatLe, 32));
    wide_mask->SetInvalidationBytes(RecordWriter::MAX_INVALIDATION_BYTES + 1);
    zassert_true(
        Throws<std::runtime_error>([&] {
            RecordWriter wide_writer(*wide_mask);
        }),
        "%zu invalidation bytes do not fit the 32 bit mask", RecordWriter::MAX_INVALIDATION_BYTES + 1
    );

    // 8 + 7 * 8 = 64 bytes fit exactly; the invalidation byte would make 65.
    auto full = MakeGroup(8, 4);
    for(size_t i = 0; i < 7; i++)
        full->AddChannel(MakeChannel(ChannelBlock::DataType::FloatLe, 64));
    zassert_equal(RecordWriter(*full).GetRecordSizeBytes(), RecordWriter::MAX_RECORD_BYTES);
    full->SetInvalidationBytes(1);
    zassert_true(
        Throws<std::runtime_error>([&] {
            RecordWriter full_writer(*full);
        }),
        "the invalidation byte counts towards the %zu byte record buffer", RecordWriter::MAX_RECORD_BYTES
    );
}

ZTEST(mdf_records, test_CanRecordWriterLayout) {
    const auto group = MakeCanGroup(2, 0x0506);
    const auto vlsd_group = MakeVlsdGroup(2, 0x0304);
    CanRecordWriter writer(*group, *vlsd_group, MdfDataType::Float64, 8);

    zassert_equal(writer.GetRecordSizeBytes(), 2 + TIME_SIZE + CAN_FRAME_SIZE);

    auto stream = MakeStream();
    CanFrame frame = MakeCanFrame(3);
    frame.data = { 0x11, 0x22, 0x33 };

    const size_t vlsd_record_size = 2 + VLSD_LENGTH_SIZE + 3;
    const size_t record_size = 2 + TIME_SIZE + CAN_FRAME_SIZE;
    zassert_equal(
        writer.Write(stream, frame, 0.5, 1), vlsd_record_size + record_size, "both records and their ids are counted"
    );

    const auto written = stream.Str();
    const auto data = AsBytes(written);
    zassert_equal(data.size(), vlsd_record_size + record_size);

    zassert_equal(Read<uint16_t>(data, 0), 0x0304, "the VLSD record comes first with its group's id");
    zassert_equal(Read<uint32_t>(data, 2), 3, "value length prefix");
    zassert_equal(data[6], 0x11);
    zassert_equal(data[8], 0x33);

    zassert_equal(Read<uint16_t>(data, vlsd_record_size), 0x0506, "the frame record follows with its group's id");
    zassert_equal(Read<double>(data, vlsd_record_size + 2), 0.5, "the time is a Float64");
    zassert_equal(data[vlsd_record_size + 2 + CAN_DATA_LENGTH], 3);
    zassert_equal(Read<uint64_t>(data, vlsd_record_size + 2 + CAN_DATA_BYTES), 0, "the first payload is at offset 0");

    zassert_equal(vlsd_group->GetCycleCount(), 1);
    zassert_equal(vlsd_group->GetVlsdDataSizeBytes(), 3, "only the value bytes count towards the total size");
    zassert_equal(group->GetCycleCount(), 1);
    zassert_equal(writer.GetBytesWritten(), vlsd_record_size + record_size);
}

ZTEST(mdf_records, test_CanRecordWriterIssuesOneWritePerFrame) {
    const auto group = MakeCanGroup(1, 2);
    const auto vlsd_group = MakeVlsdGroup(1, 1);
    CanRecordWriter writer(*group, *vlsd_group, MdfDataType::Float64, 8);

    WriteCountingStreamBuf stream;
    for(const size_t payload_size : { size_t{ 0 }, size_t{ 8 }, size_t{ 64 } })
        writer.Write(stream, MakeCanFrame(payload_size), 0.0, 1);

    zassert_equal(stream.WriteCount(), 3, "each frame and its payload must reach the stream in a single sputn()");

    const size_t record_size = 1 + TIME_SIZE + CAN_FRAME_SIZE;
    const size_t expected = 3 * (1 + VLSD_LENGTH_SIZE + record_size) + 8 + 64;
    zassert_equal(stream.Str().size(), expected, "a 64 byte payload fits the writer's buffer");
}

ZTEST(mdf_records, test_CanRecordWriterOffsetsAccumulate) {
    const auto group = MakeCanGroup(1, 2);
    const auto vlsd_group = MakeVlsdGroup(1, 1);
    CanRecordWriter writer(*group, *vlsd_group, MdfDataType::Float64, 8);

    auto stream = MakeStream();
    zassert_equal(writer.GetVlsdOffset(), 0);

    writer.Write(stream, MakeCanFrame(8), 0.0, 1);
    zassert_equal(writer.GetVlsdOffset(), 12, "the 4 byte length prefix is part of the offset, the record id is not");

    writer.Write(stream, MakeCanFrame(8), 0.1, 1);
    zassert_equal(writer.GetVlsdOffset(), 24);

    const auto written = stream.Str();
    const auto data = AsBytes(written);
    const size_t pair_size = 1 + VLSD_LENGTH_SIZE + 8 + 1 + TIME_SIZE + CAN_FRAME_SIZE;
    const size_t second_record = pair_size + 1 + VLSD_LENGTH_SIZE + 8;
    zassert_equal(
        Read<uint64_t>(data, second_record + 1 + CAN_DATA_BYTES), 12, "the second frame points at the second payload"
    );

    zassert_equal(vlsd_group->GetCycleCount(), 2);
    zassert_equal(vlsd_group->GetVlsdDataSizeBytes(), 16);
    zassert_equal(group->GetCycleCount(), 2);

    writer.Reset();
    zassert_equal(writer.GetVlsdOffset(), 0, "a new file restarts the offsets");
}

ZTEST(mdf_records, test_CanRecordWriterThrowsWhenTheOffsetNoLongerFitsFourBytes) {
    const auto group = MakeCanGroup(1, 2, 4);
    const auto vlsd_group = MakeVlsdGroup(1, 1);
    CanRecordWriter writer(*group, *vlsd_group, MdfDataType::Float64, 4);

    // The last payload whose successor still starts inside the 32 bit range.
    constexpr uint64_t START = (1ULL << 32) - 2ULL * (VLSD_LENGTH_SIZE + 8);
    writer.Reset(START);

    auto stream = MakeStream();
    writer.Write(stream, MakeCanFrame(8), 0.0, 1);

    const auto written = stream.Str();
    const auto data = AsBytes(written);
    zassert_equal(
        Read<uint32_t>(data, 1 + VLSD_LENGTH_SIZE + 8 + 1 + CAN_DATA_BYTES), static_cast<uint32_t>(START),
        "a 4 byte offset is written as the low bytes"
    );

    bool threw = false;
    try {
        writer.Write(stream, MakeCanFrame(8), 0.1, 1);
    } catch(const std::overflow_error&) {
        threw = true;
    }
    zassert_true(threw, "a truncated offset would point readers at the wrong payload");
    zassert_equal(stream.Str().size(), written.size(), "a rejected frame must not reach the stream");
    zassert_equal(writer.GetVlsdOffset(), START + VLSD_LENGTH_SIZE + 8, "a rejected frame must not advance the offset");
    zassert_equal(group->GetCycleCount(), 1);
    zassert_equal(vlsd_group->GetCycleCount(), 1);

    const auto wide_group = MakeCanGroup(1, 2, 8);
    const auto wide_vlsd_group = MakeVlsdGroup(1, 1);
    CanRecordWriter wide_writer(*wide_group, *wide_vlsd_group, MdfDataType::Float64, 8);
    wide_writer.Reset(START);

    auto wide_stream = MakeStream();
    wide_writer.Write(wide_stream, MakeCanFrame(8), 0.0, 1);
    wide_writer.Write(wide_stream, MakeCanFrame(8), 0.1, 1);
    zassert_equal(wide_writer.GetVlsdOffset(), 1ULL << 32, "an 8 byte offset goes past 4 GiB");
}

ZTEST(mdf_records, test_CanRecordWriterThrowsOnAShortWrite) {
    const auto group = MakeCanGroup(1, 2);
    const auto vlsd_group = MakeVlsdGroup(1, 1);
    CanRecordWriter writer(*group, *vlsd_group, MdfDataType::Float64, 8);

    LimitedStreamBuf stream(10);
    zassert_true(
        Throws<std::ios_base::failure>([&] {
            writer.Write(stream, MakeCanFrame(8), 0.0, 1);
        }),
        "a truncated frame must surface as an I/O failure"
    );
    zassert_equal(writer.GetVlsdOffset(), 0, "a frame that did not reach the stream does not advance the offset");
    zassert_equal(group->GetCycleCount(), 0);
    zassert_equal(vlsd_group->GetCycleCount(), 0);
}

ZTEST(mdf_records, test_CanRecordWriterRejectsMismatchedGroups) {
    auto group = MakeCanGroup(1, 2);
    auto vlsd_group = MakeVlsdGroup(1, 1);

    auto plain_group = MakeGroup(1, 3);
    zassert_true(
        Throws<std::runtime_error>([&] {
            CanRecordWriter writer(*group, *plain_group, MdfDataType::Float64, 8);
        }),
        "a group without the VLSD flag stores fixed length records"
    );

    zassert_true(
        Throws<std::runtime_error>([&] {
            CanRecordWriter writer(*group, *vlsd_group, MdfDataType::Float64, 4);
        }),
        "a group laid out for 8 byte offsets cannot take 4 byte ones"
    );

    zassert_true(
        Throws<std::runtime_error>([&] {
            CanRecordWriter writer(*group, *vlsd_group, MdfDataType::Float32, 8);
        }),
        "a group laid out for a Float64 time cannot take a Float32 one"
    );

    for(uint8_t width : { 0, 1, 2, 3, 16 }) {
        zassert_true(
            Throws<std::invalid_argument>([&] {
                CanRecordWriter writer(*group, *vlsd_group, MdfDataType::Float64, width);
            }),
            "readers only accept 4 or 8 byte VLSD offsets, not %u", width
        );
    }

    zassert_true(
        Throws<std::invalid_argument>([&] {
            CanRecordWriter writer(*group, *vlsd_group, MdfDataType::Uint64, 8);
        }),
        "a master time channel must be a float"
    );

    auto invalidated_group = MakeCanGroup(1, 4);
    invalidated_group->SetInvalidationBytes(1);
    zassert_true(
        Throws<std::runtime_error>([&] {
            CanRecordWriter writer(*invalidated_group, *vlsd_group, MdfDataType::Float64, 8);
        }),
        "a CAN record has no invalidation byte the writer would fill"
    );
}

ZTEST(mdf_records, test_CanWriterRejectsAGroupBuiltForAnotherLayout) {
    // Float64 time with 4-byte offsets and Float32 time with 8-byte offsets are both 19 bytes.
    Mdf4File file(Mdf4FileOptions{ .time_type = MdfDataType::Float64, .vlsd_offset_size_bytes = 4 });
    auto vlsd_group = file.CreateVLSDChannelGroup(1);
    auto can_group = file.CreateCanDataFrameChannelGroup(vlsd_group, 2, "can0");

    zassert_true(
        Throws<std::runtime_error>([&] {
            CanRecordWriter writer(*can_group, *vlsd_group, MdfDataType::Float32, 8);
        }),
        "a writer must not accept a group whose time and offset widths differ from its own"
    );

    const CanRecordWriter matching(*can_group, *vlsd_group, MdfDataType::Float64, 4);
    zassert_equal(matching.GetVlsdOffset(), 0, "the matching layout is accepted");
}
