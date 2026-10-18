#include <cstring>
#include <string>
#include <string_view>
#include <zephyr/ztest.h>

#include "utilities/cbor/cbor_helpers.hpp"
#include "utilities/memory/memory_resource_manager.h"

using eerie_leap::utilities::cbor::CborHelpers;
using eerie_leap::utilities::memory::Mrm;

ZTEST_SUITE(cbor_helpers, NULL, NULL, NULL, NULL, NULL);

ZTEST(cbor_helpers, test_ToZcborString) {
    static constexpr char K_TEXT[] = "sensor_1";

    const auto zstr = CborHelpers::ToZcborString(K_TEXT);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): zcbor strings are bytes, the case feeds them text
    zassert_equal_ptr(zstr.value, reinterpret_cast<const uint8_t*>(K_TEXT));
    zassert_equal(zstr.len, strlen(K_TEXT));
}

ZTEST(cbor_helpers, test_ToZcborString_empty) {
    static constexpr char K_TEXT[] = "";

    const auto zstr = CborHelpers::ToZcborString(K_TEXT);

    zassert_not_null(zstr.value);
    zassert_equal(zstr.len, 0);
}

ZTEST(cbor_helpers, test_ToZcborString_does_not_copy) {
    std::string text = "mutable";

    const auto zstr = CborHelpers::ToZcborString(text);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): zcbor strings are bytes, the case feeds them text
    zassert_equal_ptr(zstr.value, reinterpret_cast<const uint8_t*>(text.data()));
}

ZTEST(cbor_helpers, test_ToStdString) {
    static constexpr char K_TEXT[] = "temperature";

    const auto zstr = CborHelpers::ToZcborString(K_TEXT);
    const auto result = CborHelpers::ToStdString(zstr);

    zassert_equal(result.size(), strlen(K_TEXT));
    zassert_true(result == K_TEXT);
}

ZTEST(cbor_helpers, test_ToStdString_is_not_null_terminator_bound) {
    // zcbor strings are length-delimited slices of the payload, not C strings.
    static constexpr char PAYLOAD[] = "abcdefghij";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): zcbor strings are bytes, the case feeds them text
    const zcbor_string zstr{ .value = reinterpret_cast<const uint8_t*>(PAYLOAD), .len = 3 };

    const auto result = CborHelpers::ToStdString(zstr);

    zassert_equal(result.size(), 3);
    zassert_true(result == "abc");
}

ZTEST(cbor_helpers, test_ToStdString_keeps_embedded_nul) {
    static constexpr char PAYLOAD[] = { 'a', '\0', 'b' };
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): zcbor strings are bytes, the case feeds them text
    const zcbor_string zstr{ .value = reinterpret_cast<const uint8_t*>(PAYLOAD), .len = sizeof(PAYLOAD) };

    auto result = CborHelpers::ToStdString(zstr);

    zassert_equal(result.size(), 3);
    zassert_equal(result[0], 'a');
    zassert_equal(result[1], '\0');
    zassert_equal(result[2], 'b');
}

ZTEST(cbor_helpers, test_ToStdString_empty) {
    static constexpr char PAYLOAD[] = "unused";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): zcbor strings are bytes, the case feeds them text
    const zcbor_string zstr{ .value = reinterpret_cast<const uint8_t*>(PAYLOAD), .len = 0 };

    const auto result = CborHelpers::ToStdString(zstr);

    zassert_true(result.empty());
}

ZTEST(cbor_helpers, test_ToPmrString) {
    static constexpr char K_TEXT[] = "pressure_kpa";
    auto* mr = Mrm::GetDefaultPmr();

    const auto zstr = CborHelpers::ToZcborString(K_TEXT);
    const auto result = CborHelpers::ToPmrString(mr, zstr);

    zassert_equal(result.size(), strlen(K_TEXT));
    zassert_true(result == K_TEXT);
    zassert_true(result.get_allocator().resource()->is_equal(*mr));
}

ZTEST(cbor_helpers, test_ToPmrString_copies_payload) {
    char payload[] = "original";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): zcbor strings are bytes, the case feeds them text
    const zcbor_string zstr{ .value = reinterpret_cast<const uint8_t*>(payload), .len = strlen(payload) };

    const auto result = CborHelpers::ToPmrString(Mrm::GetDefaultPmr(), zstr);
    payload[0] = 'X';

    zassert_true(result == "original");
}

ZTEST(cbor_helpers, test_round_trip) {
    static constexpr char K_TEXT[] = "a rather long identifier that exceeds the short string buffer";

    const auto zstr = CborHelpers::ToZcborString(K_TEXT);
    const auto std_string = CborHelpers::ToStdString(zstr);
    const auto zstr_again = CborHelpers::ToZcborString(std_string);
    auto pmr_string = CborHelpers::ToPmrString(Mrm::GetDefaultPmr(), zstr_again);

    zassert_equal(zstr.len, zstr_again.len);
    zassert_equal(pmr_string.size(), strlen(K_TEXT));
    zassert_mem_equal(pmr_string.data(), K_TEXT, strlen(K_TEXT));
}
