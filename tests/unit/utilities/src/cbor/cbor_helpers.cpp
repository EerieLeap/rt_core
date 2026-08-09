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
    static constexpr char kText[] = "sensor_1";

    auto zstr = CborHelpers::ToZcborString(kText);

    zassert_equal_ptr(zstr.value, reinterpret_cast<const uint8_t*>(kText));
    zassert_equal(zstr.len, strlen(kText));
}

ZTEST(cbor_helpers, test_ToZcborString_empty) {
    static constexpr char kText[] = "";

    auto zstr = CborHelpers::ToZcborString(kText);

    zassert_not_null(zstr.value);
    zassert_equal(zstr.len, 0);
}

ZTEST(cbor_helpers, test_ToZcborString_does_not_copy) {
    std::string text = "mutable";

    auto zstr = CborHelpers::ToZcborString(text);

    zassert_equal_ptr(zstr.value, reinterpret_cast<const uint8_t*>(text.data()));
}

ZTEST(cbor_helpers, test_ToStdString) {
    static constexpr char kText[] = "temperature";

    auto zstr = CborHelpers::ToZcborString(kText);
    auto result = CborHelpers::ToStdString(zstr);

    zassert_equal(result.size(), strlen(kText));
    zassert_true(result == kText);
}

ZTEST(cbor_helpers, test_ToStdString_is_not_null_terminator_bound) {
    // zcbor strings are length-delimited slices of the payload, not C strings.
    static constexpr char kPayload[] = "abcdefghij";
    zcbor_string zstr {
        .value = reinterpret_cast<const uint8_t*>(kPayload),
        .len = 3
    };

    auto result = CborHelpers::ToStdString(zstr);

    zassert_equal(result.size(), 3);
    zassert_true(result == "abc");
}

ZTEST(cbor_helpers, test_ToStdString_keeps_embedded_nul) {
    static constexpr char kPayload[] = { 'a', '\0', 'b' };
    zcbor_string zstr {
        .value = reinterpret_cast<const uint8_t*>(kPayload),
        .len = sizeof(kPayload)
    };

    auto result = CborHelpers::ToStdString(zstr);

    zassert_equal(result.size(), 3);
    zassert_equal(result[0], 'a');
    zassert_equal(result[1], '\0');
    zassert_equal(result[2], 'b');
}

ZTEST(cbor_helpers, test_ToStdString_empty) {
    static constexpr char kPayload[] = "unused";
    zcbor_string zstr {
        .value = reinterpret_cast<const uint8_t*>(kPayload),
        .len = 0
    };

    auto result = CborHelpers::ToStdString(zstr);

    zassert_true(result.empty());
}

ZTEST(cbor_helpers, test_ToPmrString) {
    static constexpr char kText[] = "pressure_kpa";
    auto* mr = Mrm::GetDefaultPmr();

    auto zstr = CborHelpers::ToZcborString(kText);
    auto result = CborHelpers::ToPmrString(mr, zstr);

    zassert_equal(result.size(), strlen(kText));
    zassert_true(result == kText);
    zassert_true(result.get_allocator().resource()->is_equal(*mr));
}

ZTEST(cbor_helpers, test_ToPmrString_copies_payload) {
    char payload[] = "original";
    zcbor_string zstr {
        .value = reinterpret_cast<const uint8_t*>(payload),
        .len = strlen(payload)
    };

    auto result = CborHelpers::ToPmrString(Mrm::GetDefaultPmr(), zstr);
    payload[0] = 'X';

    zassert_true(result == "original");
}

ZTEST(cbor_helpers, test_round_trip) {
    static constexpr char kText[] = "a rather long identifier that exceeds the short string buffer";

    auto zstr = CborHelpers::ToZcborString(kText);
    auto std_string = CborHelpers::ToStdString(zstr);
    auto zstr_again = CborHelpers::ToZcborString(std_string);
    auto pmr_string = CborHelpers::ToPmrString(Mrm::GetDefaultPmr(), zstr_again);

    zassert_equal(zstr.len, zstr_again.len);
    zassert_equal(pmr_string.size(), strlen(kText));
    zassert_mem_equal(pmr_string.data(), kText, strlen(kText));
}
