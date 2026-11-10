#include <zephyr/ztest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <span>

#include "domain/can_monitor_domain/utilities/leb128.h"

using namespace eerie_leap::domain::can_monitor_domain::utilities;

ZTEST_SUITE(can_monitor_leb128, NULL, NULL, NULL, NULL, NULL);

static void ExpectEncoding(uint64_t value, std::initializer_list<uint8_t> expected) {
    std::array<uint8_t, LEB128_MAX_SIZE> buffer{};

    const size_t size = EncodeLeb128(value, buffer);
    zassert_equal(size, expected.size());
    zassert_equal(Leb128Size(value), expected.size());

    size_t i = 0;
    for(const uint8_t byte : expected)
        zassert_equal(buffer[i++], byte);

    const auto decoded = DecodeLeb128(std::span<const uint8_t>(buffer).first(size));
    zassert_true(decoded.Ok());
    zassert_equal(decoded.value, value);
    zassert_equal(decoded.size, size);
}

ZTEST(can_monitor_leb128, test_LimitsEncodeAndDecode) {
    ExpectEncoding(0, { 0x00 });
    ExpectEncoding(1, { 0x01 });
    ExpectEncoding(127, { 0x7F });
    ExpectEncoding(128, { 0x80, 0x01 });
    ExpectEncoding(16383, { 0xFF, 0x7F });
    ExpectEncoding(16384, { 0x80, 0x80, 0x01 });
    ExpectEncoding(500000, { 0xA0, 0xC2, 0x1E });
    ExpectEncoding(uint64_t{ 1 } << 63, { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01 });
    ExpectEncoding(UINT64_MAX, { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01 });
}

ZTEST(can_monitor_leb128, test_EveryPowerOfTwoRoundTrips) {
    for(unsigned shift = 0; shift < 64; ++shift) {
        for(const uint64_t value : std::initializer_list<uint64_t>{
                uint64_t{ 1 } << shift, (uint64_t{ 1 } << shift) - 1, (uint64_t{ 1 } << shift) + 1 }) {
            std::array<uint8_t, LEB128_MAX_SIZE> buffer{};
            const size_t size = EncodeLeb128(value, buffer);
            zassert_true(size >= 1 && size <= LEB128_MAX_SIZE);

            const auto decoded = DecodeLeb128(std::span<const uint8_t>(buffer).first(size));
            zassert_true(decoded.Ok());
            zassert_equal(decoded.value, value);
            zassert_equal(decoded.size, size);
        }
    }
}

ZTEST(can_monitor_leb128, test_EncodeFailsWhenItDoesNotFit) {
    std::array<uint8_t, LEB128_MAX_SIZE> buffer{};

    zassert_equal(EncodeLeb128(128, std::span<uint8_t>(buffer).first(1)), 0U);
    zassert_equal(EncodeLeb128(0, std::span<uint8_t>(buffer).first(0)), 0U);
    zassert_equal(EncodeLeb128(UINT64_MAX, std::span<uint8_t>(buffer).first(9)), 0U);
    zassert_equal(EncodeLeb128(UINT64_MAX, std::span<uint8_t>(buffer).first(10)), 10U);
}

ZTEST(can_monitor_leb128, test_DecodeStopsAtTheEndOfTheValue) {
    const std::array<uint8_t, 4> bytes = { 0x80, 0x01, 0x55, 0x66 };

    const auto decoded = DecodeLeb128(bytes);
    zassert_true(decoded.Ok());
    zassert_equal(decoded.value, 128U);
    zassert_equal(decoded.size, 2U);
}

ZTEST(can_monitor_leb128, test_DecodeReportsTruncation) {
    const std::array<uint8_t, 3> bytes = { 0x80, 0x80, 0x80 };

    zassert_true(DecodeLeb128(std::span<const uint8_t>()).status == Leb128Status::TRUNCATED);
    zassert_true(DecodeLeb128(std::span<const uint8_t>(bytes).first(1)).status == Leb128Status::TRUNCATED);
    zassert_true(DecodeLeb128(bytes).status == Leb128Status::TRUNCATED);
    zassert_false(DecodeLeb128(bytes).Ok());

    // Nine continuation bytes: still short of the tenth.
    const std::array<uint8_t, 9> nine = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    zassert_true(DecodeLeb128(nine).status == Leb128Status::TRUNCATED);
}

ZTEST(can_monitor_leb128, test_DecodeRejectsMoreThanTenBytes) {
    const std::array<uint8_t, 11> eleven = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01 };

    zassert_true(DecodeLeb128(eleven).status == Leb128Status::INVALID);

    // Ten bytes that all continue.
    const std::array<uint8_t, 10> ten = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    zassert_true(DecodeLeb128(ten).status == Leb128Status::INVALID);
}

ZTEST(can_monitor_leb128, test_DecodeRejectsOverflowPast64Bits) {
    // The tenth byte may carry bit 63 only.
    const std::array<uint8_t, 10> overflow = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02 };
    zassert_true(DecodeLeb128(overflow).status == Leb128Status::INVALID);

    const std::array<uint8_t, 10> fits = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01 };
    const auto decoded = DecodeLeb128(fits);
    zassert_true(decoded.Ok());
    zassert_equal(decoded.value, UINT64_MAX);
}

ZTEST(can_monitor_leb128, test_DecodeAcceptsPaddedValues) {
    const std::array<uint8_t, 3> padded = { 0x81, 0x80, 0x00 };

    const auto decoded = DecodeLeb128(padded);
    zassert_true(decoded.Ok());
    zassert_equal(decoded.value, 1U);
    zassert_equal(decoded.size, 3U);
}
