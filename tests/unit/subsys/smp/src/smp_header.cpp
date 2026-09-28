#include <array>
#include <cstdint>

#include <zephyr/ztest.h>

#include "subsys/smp/smp_header.h"

using eerie_leap::subsys::smp::SmpHeader;
using eerie_leap::subsys::smp::SmpOperation;

ZTEST_SUITE(smp_header, NULL, NULL, NULL, NULL, NULL);

ZTEST(smp_header, test_encode_matches_the_wire_layout) {
    const SmpHeader header{
        .operation = SmpOperation::WRITE,
        .version = 1,
        .flags = 0,
        .length = 0x0102,
        .group = 0x0040,
        .sequence = 9,
        .command_id = 3,
    };

    std::array<uint8_t, SmpHeader::SIZE> data{};
    header.Encode(data);

    constexpr std::array<uint8_t, SmpHeader::SIZE> expected = {0x0A, 0x00, 0x01, 0x02, 0x00, 0x40, 0x09, 0x03};
    zassert_mem_equal(data.data(), expected.data(), expected.size());
}

ZTEST(smp_header, test_parse_round_trips) {
    constexpr std::array<uint8_t, SmpHeader::SIZE> data = {0x0B, 0x00, 0x00, 0x05, 0x00, 0x00, 0x2A, 0x00};

    auto header = SmpHeader::Parse(data);

    zassert_true(header.has_value());
    zassert_equal(header->operation, SmpOperation::WRITE_RESPONSE);
    zassert_equal(header->version, 1);
    zassert_equal(header->length, 5);
    zassert_equal(header->group, 0);
    zassert_equal(header->sequence, 0x2A);
    zassert_equal(header->GetPacketSize(), 13);
    zassert_true(header->IsResponse());
    zassert_false(header->IsRequest());
}

ZTEST(smp_header, test_short_input_is_rejected) {
    constexpr std::array<uint8_t, SmpHeader::SIZE - 1> data{};

    zassert_false(SmpHeader::Parse(data).has_value());
}

ZTEST(smp_header, test_read_and_write_are_requests) {
    zassert_true(SmpHeader{.operation = SmpOperation::READ}.IsRequest());
    zassert_true(SmpHeader{.operation = SmpOperation::WRITE}.IsRequest());
    zassert_true(SmpHeader{.operation = SmpOperation::READ_RESPONSE}.IsResponse());
    zassert_false(SmpHeader{.operation = static_cast<SmpOperation>(5)}.IsRequest());
    zassert_false(SmpHeader{.operation = static_cast<SmpOperation>(5)}.IsResponse());
}
