#include <zephyr/ztest.h>

#include <array>
#include <cstdint>

#include "utilities/memory/memory_resource_manager.h"
#include "domain/canbus_domain/utilities/can_signal_codec.h"

using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::domain::canbus_domain::models;
using namespace eerie_leap::domain::canbus_domain::utilities;

ZTEST_SUITE(can_signal_codec, NULL, NULL, NULL, NULL, NULL);

static CanSignalConfiguration can_signal_codec_CreateSignal(
    uint32_t start_bit,
    uint32_t size_bits,
    CanSignalByteOrder byte_order,
    bool is_signed,
    float factor = 1.0F,
    float offset = 0.0F) {

    CanSignalConfiguration signal(std::allocator_arg, Mrm::GetDefaultPmr());
    signal.start_bit = start_bit;
    signal.size_bits = size_bits;
    signal.byte_order = byte_order;
    signal.is_signed = is_signed;
    signal.factor = factor;
    signal.offset = offset;
    signal.SetName("test_signal");

    return signal;
}

ZTEST(can_signal_codec, test_DecodeLittleEndianUnsigned) {
    auto signal = can_signal_codec_CreateSignal(8, 16, CanSignalByteOrder::LITTLE_ENDIAN_INTEL, false);
    std::array<uint8_t, 8> data = {0x00, 0x34, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00};

    auto value = CanSignalCodec::Decode(signal, data);

    zassert_true(value.has_value());
    zassert_equal(value.value(), 0x1234);
}

ZTEST(can_signal_codec, test_DecodeBigEndianUnsigned) {
    auto signal = can_signal_codec_CreateSignal(7, 16, CanSignalByteOrder::BIG_ENDIAN_MOTOROLA, false);
    std::array<uint8_t, 8> data = {0x12, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    auto value = CanSignalCodec::Decode(signal, data);

    zassert_true(value.has_value());
    zassert_equal(value.value(), 0x1234);
}

ZTEST(can_signal_codec, test_DecodeSignedIsSignExtended) {
    auto signal = can_signal_codec_CreateSignal(0, 8, CanSignalByteOrder::LITTLE_ENDIAN_INTEL, true);
    std::array<uint8_t, 8> data = {0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    auto value = CanSignalCodec::Decode(signal, data);

    zassert_true(value.has_value());
    zassert_equal(value.value(), -1.0F);
}

ZTEST(can_signal_codec, test_DecodeAppliesFactorAndOffset) {
    auto signal = can_signal_codec_CreateSignal(0, 8, CanSignalByteOrder::LITTLE_ENDIAN_INTEL, false, 0.5F, -40.0F);
    std::array<uint8_t, 8> data = {100, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    auto value = CanSignalCodec::Decode(signal, data);

    zassert_true(value.has_value());
    zassert_equal(value.value(), 10.0F);
}

ZTEST(can_signal_codec, test_EncodeLittleEndianWritesOnlySignalBits) {
    auto signal = can_signal_codec_CreateSignal(8, 16, CanSignalByteOrder::LITTLE_ENDIAN_INTEL, false);
    std::array<uint8_t, 8> data = {0xAA, 0x00, 0x00, 0xAA, 0x00, 0x00, 0x00, 0x00};

    zassert_true(CanSignalCodec::Encode(signal, 0x1234, data));

    zassert_equal(data[0], 0xAA);
    zassert_equal(data[1], 0x34);
    zassert_equal(data[2], 0x12);
    zassert_equal(data[3], 0xAA);
}

ZTEST(can_signal_codec, test_EncodeBigEndianWritesOnlySignalBits) {
    auto signal = can_signal_codec_CreateSignal(7, 16, CanSignalByteOrder::BIG_ENDIAN_MOTOROLA, false);
    std::array<uint8_t, 8> data = {0x00, 0x00, 0xAA, 0x00, 0x00, 0x00, 0x00, 0x00};

    zassert_true(CanSignalCodec::Encode(signal, 0x1234, data));

    zassert_equal(data[0], 0x12);
    zassert_equal(data[1], 0x34);
    zassert_equal(data[2], 0xAA);
}

ZTEST(can_signal_codec, test_EncodeDecodeRoundTrip) {
    const std::array byte_orders = {
        CanSignalByteOrder::LITTLE_ENDIAN_INTEL,
        CanSignalByteOrder::BIG_ENDIAN_MOTOROLA
    };

    for(auto byte_order : byte_orders) {
        auto signal = can_signal_codec_CreateSignal(
            byte_order == CanSignalByteOrder::BIG_ENDIAN_MOTOROLA ? 15 : 8,
            12,
            byte_order,
            true,
            0.25F,
            -100.0F);

        std::array<uint8_t, 8> data = {};

        zassert_true(CanSignalCodec::Encode(signal, -60.5F, data));

        auto value = CanSignalCodec::Decode(signal, data);

        zassert_true(value.has_value());
        zassert_equal(value.value(), -60.5F);
    }
}

ZTEST(can_signal_codec, test_EncodeSaturatesOutOfRangeValues) {
    auto signal = can_signal_codec_CreateSignal(0, 8, CanSignalByteOrder::LITTLE_ENDIAN_INTEL, false);
    std::array<uint8_t, 8> data = {};

    zassert_true(CanSignalCodec::Encode(signal, 1000.0F, data));
    zassert_equal(data[0], 0xFF);

    zassert_true(CanSignalCodec::Encode(signal, -1000.0F, data));
    zassert_equal(data[0], 0x00);
}

ZTEST(can_signal_codec, test_RejectsSignalExceedingData) {
    auto signal = can_signal_codec_CreateSignal(56, 16, CanSignalByteOrder::LITTLE_ENDIAN_INTEL, false);
    std::array<uint8_t, 8> data = {};

    zassert_false(CanSignalCodec::IsLayoutValid(signal, data.size()));
    zassert_false(CanSignalCodec::Decode(signal, data).has_value());
    zassert_false(CanSignalCodec::Encode(signal, 1.0F, data));
}
