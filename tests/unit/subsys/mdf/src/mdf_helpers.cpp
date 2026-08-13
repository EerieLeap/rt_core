#include <cstdint>
#include <iterator>
#include <stdexcept>

#include <zephyr/ztest.h>

#include "subsys/mdf/mdf4/channel_block.h"
#include "subsys/mdf/mdf_data_type.h"
#include "subsys/mdf/mdf_helpers.h"
#include "subsys/mdf/mdf_value.h"

using namespace eerie_leap::subsys::mdf;
using eerie_leap::subsys::mdf::mdf4::ChannelBlock;

// The mapping is constexpr, so a regression breaks the build rather than a test run.
static_assert(GetMdfDataType(MdfValue{int32_t{}}) == MdfDataType::Int32);
static_assert(GetMdfDataType(MdfValue{int64_t{}}) == MdfDataType::Int64);
static_assert(GetMdfDataType(MdfValue{uint32_t{}}) == MdfDataType::Uint32);
static_assert(GetMdfDataType(MdfValue{uint64_t{}}) == MdfDataType::Uint64);
static_assert(GetMdfDataType(MdfValue{float{}}) == MdfDataType::Float32);
static_assert(GetMdfDataType(MdfValue{double{}}) == MdfDataType::Float64);

ZTEST_SUITE(mdf_helpers, NULL, NULL, NULL, NULL, NULL);

ZTEST(mdf_helpers, test_ChannelDataTypeMapping) {
    const struct {
        MdfDataType data_type;
        ChannelBlock::DataType expected_type;
        uint32_t expected_bit_count;
    } cases[] = {
        {MdfDataType::Int32, ChannelBlock::DataType::SignedIntegerLe, 32},
        {MdfDataType::Int64, ChannelBlock::DataType::SignedIntegerLe, 64},
        {MdfDataType::Uint32, ChannelBlock::DataType::UnsignedIntegerLe, 32},
        {MdfDataType::Uint64, ChannelBlock::DataType::UnsignedIntegerLe, 64},
        {MdfDataType::Float32, ChannelBlock::DataType::FloatLe, 32},
        {MdfDataType::Float64, ChannelBlock::DataType::FloatLe, 64},
        {MdfDataType::ByteArray, ChannelBlock::DataType::ByteArray, 0},
    };

    for(const auto& test_case : cases) {
        const auto mapped = MdfHelpers::ToMdf4ChannelDataType(test_case.data_type);

        zassert_equal(mapped.data_type, test_case.expected_type,
            "data type %u", static_cast<unsigned>(test_case.data_type));
        zassert_equal(mapped.bit_count, test_case.expected_bit_count,
            "bit count for data type %u", static_cast<unsigned>(test_case.data_type));
    }
}

ZTEST(mdf_helpers, test_ChannelDataTypeMappingRejectsUnsupportedTypes) {
    for(auto data_type : {MdfDataType::None, static_cast<MdfDataType>(200)}) {
        bool threw = false;
        try {
            MdfHelpers::ToMdf4ChannelDataType(data_type);
        } catch(const std::runtime_error&) {
            threw = true;
        }
        zassert_true(threw, "data type %u has no channel representation", static_cast<unsigned>(data_type));
    }
}

ZTEST(mdf_helpers, test_ValueTypeMatchesTheChannelItIsWrittenTo) {
    // Every value alternative has to resolve to a channel whose width fits the value.
    const MdfValue values[] = {
        int32_t{-1}, int64_t{-1}, uint32_t{1}, uint64_t{1}, 1.0F, 1.0};

    const size_t expected_sizes[] = {4, 8, 4, 8, 4, 8};

    for(size_t i = 0; i < std::size(values); i++) {
        const auto mapped = MdfHelpers::ToMdf4ChannelDataType(GetMdfDataType(values[i]));
        zassert_equal(mapped.bit_count / 8, expected_sizes[i], "value alternative %zu", i);
    }
}
