#include <array>
#include <cstdint>
#include <ios>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <zephyr/ztest.h>

#include "subsys/mdf/mdf4/channel_block.h"
#include "subsys/mdf/mdf4/channel_group_block.h"
#include "subsys/mdf/mdf4/data_record.h"
#include "subsys/mdf/mdf4/vlsd_data_record.h"
#include "subsys/mdf/mdf_value.h"

#include "mdf_test_support.h"

using namespace eerie_leap::subsys::mdf;
using namespace eerie_leap::subsys::mdf::mdf4;
using namespace mdf_test;

namespace {

std::shared_ptr<ChannelBlock> MakeChannel(ChannelBlock::DataType data_type, uint32_t bit_count) {
    return std::make_shared<ChannelBlock>(
        ChannelBlock::Type::FixedLength, ChannelBlock::SyncType::NoSync, data_type, bit_count);
}

std::shared_ptr<ChannelGroupBlock> MakeGroup(uint8_t record_id_size, uint64_t record_id) {
    return std::make_shared<ChannelGroupBlock>(record_id_size, record_id);
}

std::shared_ptr<ChannelGroupBlock> MakeVlsdGroup(uint8_t record_id_size, uint64_t record_id) {
    auto group = MakeGroup(record_id_size, record_id);
    group->SetFlags(std::to_underlying(ChannelGroupBlock::Flag::VlsdChannel));

    return group;
}

} // namespace

ZTEST_SUITE(mdf_records, NULL, NULL, NULL, NULL, NULL);

ZTEST(mdf_records, test_DataRecordWritesEveryValueAtItsChannelOffset) {
    auto group = MakeGroup(2, 0x0102);
    group->AddChannel(MakeChannel(ChannelBlock::DataType::FloatLe, 32));
    group->AddChannel(MakeChannel(ChannelBlock::DataType::UnsignedIntegerLe, 64));
    group->AddChannel(MakeChannel(ChannelBlock::DataType::SignedIntegerLe, 32));

    DataRecord record(group);
    zassert_equal(record.GetRecordSizeBytes(), 2 + 16);

    auto stream = MakeStream();
    std::array<MdfValue, 3> values{1.5F, uint64_t{0xAABBCCDDEEFF0011ULL}, int32_t{-2}};

    zassert_equal(record.WriteToStream(stream, values), 18, "record id bytes are part of the record");

    const auto written = stream.str();
    const auto data = AsBytes(written);
    zassert_equal(data.size(), 18);
    zassert_equal(Read<uint16_t>(data, 0), 0x0102, "record id prefix");
    zassert_equal(Read<float>(data, 2), 1.5F);
    zassert_equal(Read<uint64_t>(data, 6), 0xAABBCCDDEEFF0011ULL);
    zassert_equal(Read<int32_t>(data, 14), -2);
}

ZTEST(mdf_records, test_DataRecordSupportsEveryValueType) {
    auto group = MakeGroup(1, 1);
    group->AddChannel(MakeChannel(ChannelBlock::DataType::SignedIntegerLe, 32));
    group->AddChannel(MakeChannel(ChannelBlock::DataType::SignedIntegerLe, 64));
    group->AddChannel(MakeChannel(ChannelBlock::DataType::UnsignedIntegerLe, 32));
    group->AddChannel(MakeChannel(ChannelBlock::DataType::UnsignedIntegerLe, 64));
    group->AddChannel(MakeChannel(ChannelBlock::DataType::FloatLe, 32));
    group->AddChannel(MakeChannel(ChannelBlock::DataType::FloatLe, 64));

    DataRecord record(group);
    auto stream = MakeStream();

    std::array<MdfValue, 6> values{
        int32_t{-1}, int64_t{-2}, uint32_t{3}, uint64_t{4}, 5.5F, 6.5};

    record.WriteToStream(stream, values);

    const auto written = stream.str();
    const auto data = AsBytes(written);
    zassert_equal(data.size(), 1 + 4 + 8 + 4 + 8 + 4 + 8);
    zassert_equal(Read<int32_t>(data, 1), -1);
    zassert_equal(Read<int64_t>(data, 5), -2);
    zassert_equal(Read<uint32_t>(data, 13), 3);
    zassert_equal(Read<uint64_t>(data, 17), 4);
    zassert_equal(Read<float>(data, 25), 5.5F);
    zassert_equal(Read<double>(data, 29), 6.5);
}

ZTEST(mdf_records, test_DataRecordCountsCycles) {
    auto group = MakeGroup(1, 1);
    group->AddChannel(MakeChannel(ChannelBlock::DataType::FloatLe, 32));

    DataRecord record(group);
    auto stream = MakeStream();

    std::array<MdfValue, 1> values{0.0F};
    for(int i = 0; i < 5; i++)
        record.WriteToStream(stream, values);

    zassert_equal(group->GetCycleCount(), 5, "cg_cycle_count has to follow the records that were written");
}

ZTEST(mdf_records, test_DataRecordRejectsWrongValueCountAndType) {
    auto group = MakeGroup(1, 1);
    group->AddChannel(MakeChannel(ChannelBlock::DataType::FloatLe, 32));
    group->AddChannel(MakeChannel(ChannelBlock::DataType::UnsignedIntegerLe, 64));

    DataRecord record(group);
    auto stream = MakeStream();

    std::array<MdfValue, 1> too_few{0.0F};
    zassert_true(Throws<std::runtime_error>([&] { record.WriteToStream(stream, too_few); }), "too few values");

    std::array<MdfValue, 3> too_many{0.0F, uint64_t{0}, uint64_t{0}};
    zassert_true(Throws<std::runtime_error>([&] { record.WriteToStream(stream, too_many); }), "too many values");

    // A 32 bit value in a 64 bit channel would leave half the field uninitialised.
    std::array<MdfValue, 2> narrow{0.0F, uint32_t{0}};
    zassert_true(Throws<std::runtime_error>([&] { record.WriteToStream(stream, narrow); }), "uint32 into a uint64 channel");

    std::array<MdfValue, 2> signedness{0.0F, int64_t{0}};
    zassert_true(Throws<std::runtime_error>([&] { record.WriteToStream(stream, signedness); }), "int64 into a uint64 channel");

    std::array<MdfValue, 2> floatness{0.0, uint64_t{0}};
    zassert_true(Throws<std::runtime_error>([&] { record.WriteToStream(stream, floatness); }), "double into a float channel");

    zassert_equal(stream.str().size(), 0, "a rejected record must not reach the stream");
    zassert_equal(group->GetCycleCount(), 0);
}

ZTEST(mdf_records, test_DataRecordRequiresAChannelGroup) {
    bool threw = false;
    try {
        DataRecord record(nullptr);
    } catch(const std::invalid_argument&) {
        threw = true;
    }
    zassert_true(threw);
}

ZTEST(mdf_records, test_RawRecordIncludesTheRecordIdInTheReturnedSize) {
    auto group = MakeGroup(4, 0xDEADBEEF);
    group->AddChannel(MakeChannel(ChannelBlock::DataType::ByteArray, 80));

    DataRecord record(group);
    auto stream = MakeStream();

    const std::vector<uint8_t> payload(10, 0x7E);
    const auto bytes_written = record.WriteRawToStream(stream, payload);

    zassert_equal(bytes_written, 4 + 10, "the record id must be counted, not dropped");
    zassert_equal(stream.str().size(), bytes_written);

    const auto written = stream.str();
    const auto data = AsBytes(written);
    zassert_equal(Read<uint32_t>(data, 0), 0xDEADBEEF);
    zassert_equal(data[4], 0x7E);
    zassert_equal(group->GetCycleCount(), 1);
}

ZTEST(mdf_records, test_RawRecordRejectsAMismatchedDataSize) {
    auto group = MakeGroup(1, 1);
    group->AddChannel(MakeChannel(ChannelBlock::DataType::ByteArray, 80));

    DataRecord record(group);
    auto stream = MakeStream();

    const std::vector<uint8_t> too_short(9, 0);
    zassert_true(Throws<std::runtime_error>([&] { record.WriteRawToStream(stream, too_short); }));

    const std::vector<uint8_t> too_long(11, 0);
    zassert_true(Throws<std::runtime_error>([&] { record.WriteRawToStream(stream, too_long); }));

    zassert_equal(stream.str().size(), 0);
    zassert_equal(group->GetCycleCount(), 0);
}

ZTEST(mdf_records, test_VlsdRecordLayout) {
    auto group = MakeVlsdGroup(2, 0x0304);
    VlsdDataRecord record(group, 4);

    auto stream = MakeStream();
    const std::vector<uint8_t> payload{0x11, 0x22, 0x33};

    zassert_equal(record.WriteToStream(stream, payload), 2 + 4 + 3);

    const auto written = stream.str();
    const auto data = AsBytes(written);
    zassert_equal(Read<uint16_t>(data, 0), 0x0304, "record id prefix");
    zassert_equal(Read<uint32_t>(data, 2), 3, "value length prefix");
    zassert_equal(data[6], 0x11);
    zassert_equal(data[8], 0x33);

    zassert_equal(group->GetCycleCount(), 1);
    zassert_equal(group->GetVlsdDataSizeBytes(), 3, "only the value bytes count towards the total size");
}

ZTEST(mdf_records, test_VlsdOffsetAccumulatesExcludingTheRecordId) {
    auto group = MakeVlsdGroup(1, 1);
    VlsdDataRecord record(group, 4);

    auto stream = MakeStream();
    const std::vector<uint8_t> payload(8, 0xAA);

    zassert_equal(record.GetOffset(), 0);
    zassert_equal(Read<uint32_t>(record.GetOffsetData(), 0), 0);

    record.WriteToStream(stream, payload);
    zassert_equal(record.GetOffset(), 12, "the 4 byte length prefix is part of the offset");
    zassert_equal(Read<uint32_t>(record.GetOffsetData(), 0), 12);

    record.WriteToStream(stream, payload);
    zassert_equal(record.GetOffset(), 24);

    record.Reset();
    zassert_equal(record.GetOffset(), 0, "a new file restarts the offsets");
    zassert_equal(Read<uint32_t>(record.GetOffsetData(), 0), 0);
}

ZTEST(mdf_records, test_VlsdOffsetDataMatchesTheOffsetChannelWidth) {
    for(uint64_t width : {1, 2, 4, 8}) {
        auto group = MakeVlsdGroup(1, 1);
        VlsdDataRecord record(group, width);

        auto offset_data = record.GetOffsetData();
        zassert_equal(offset_data.size(), width);

        auto stream = MakeStream();
        record.WriteToStream(stream, std::vector<uint8_t>(4, 0));

        offset_data = record.GetOffsetData();
        zassert_equal(offset_data[0], 8, "little endian low byte of the offset");
        for(size_t i = 1; i < offset_data.size(); i++)
            zassert_equal(offset_data[i], 0);
    }
}

ZTEST(mdf_records, test_VlsdRejectsAnInvalidOffsetWidth) {
    for(uint64_t width : {0, 3, 5, 16}) {
        auto group = MakeVlsdGroup(1, 1);
        zassert_true(Throws<std::runtime_error>([&] { VlsdDataRecord record(group, width); }),
            "offset width %llu should be rejected", static_cast<unsigned long long>(width));
    }
}

ZTEST(mdf_records, test_VlsdRejectsANonVlsdChannelGroup) {
    auto group = MakeGroup(1, 1);
    zassert_true(Throws<std::runtime_error>([&] { VlsdDataRecord record(group, 4); }),
        "a group without the VLSD flag stores fixed length records");

    bool threw = false;
    try {
        VlsdDataRecord record(nullptr, 4);
    } catch(const std::invalid_argument&) {
        threw = true;
    }
    zassert_true(threw);
}

ZTEST(mdf_records, test_VlsdThrowsWhenTheOffsetNoLongerFitsTheChannel) {
    auto group = MakeVlsdGroup(1, 1);
    VlsdDataRecord record(group, 1); // an 8 bit offset channel overflows after 255 bytes

    auto stream = MakeStream();
    const std::vector<uint8_t> payload(96, 0);

    record.WriteToStream(stream, payload);
    record.WriteToStream(stream, payload);
    zassert_equal(record.GetOffset(), 200);

    bool threw = false;
    try {
        record.WriteToStream(stream, payload);
    } catch(const std::overflow_error&) {
        threw = true;
    }
    zassert_true(threw, "a truncated offset would point readers at the wrong value");
    zassert_equal(record.GetOffset(), 200, "a rejected record must not advance the offset");
    zassert_equal(group->GetCycleCount(), 2);
}
