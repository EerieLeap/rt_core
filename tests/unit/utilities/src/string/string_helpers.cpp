#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <zephyr/ztest.h>

#include "utilities/string/string_helpers.h"

using eerie_leap::utilities::string::StringHelpers;

ZTEST_SUITE(string_helpers, NULL, NULL, NULL, NULL, NULL);

ZTEST(string_helpers, test_ToPaddedCharArray_pads_with_explicit_char) {
    auto padded = StringHelpers::ToPaddedCharArray("abc", 8, '.');

    zassert_not_null(padded.get());
    zassert_mem_equal(padded.get(), "abc.....", 8);
}

ZTEST(string_helpers, test_ToPaddedCharArray_pads_with_space_by_default) {
    auto padded = StringHelpers::ToPaddedCharArray("abc", 6);

    zassert_mem_equal(padded.get(), "abc   ", 6);
}

ZTEST(string_helpers, test_ToPaddedCharArray_exact_fit_has_no_padding) {
    auto padded = StringHelpers::ToPaddedCharArray("abcdef", 6, '.');

    zassert_mem_equal(padded.get(), "abcdef", 6);
}

ZTEST(string_helpers, test_ToPaddedCharArray_empty_input_is_all_padding) {
    auto padded = StringHelpers::ToPaddedCharArray("", 4, 'x');

    zassert_mem_equal(padded.get(), "xxxx", 4);
}

ZTEST(string_helpers, test_ToPaddedCharArray_returns_independent_buffers) {
    std::string source = "abc";

    auto first = StringHelpers::ToPaddedCharArray(source, 8, '.');
    auto second = StringHelpers::ToPaddedCharArray(source, 8, '.');

    zassert_true(first.get() != second.get());

    first[0] = 'z';
    zassert_equal(second[0], 'a');
}

ZTEST(string_helpers, test_GetHash_is_deterministic) {
    zassert_equal(StringHelpers::GetHash("sensor_1"), StringHelpers::GetHash("sensor_1"));
    zassert_equal(StringHelpers::GetHash(""), StringHelpers::GetHash(""));
}

ZTEST(string_helpers, test_GetHash_matches_known_xxhash64_values) {
    // Expected values are the lower 32 bits of XXH64(seed=0) for these inputs.
    zassert_equal(StringHelpers::GetHash("sensor_1"), 0x479b7bc9u);
    zassert_equal(StringHelpers::GetHash(""), 0x51d8e999u);
    zassert_equal(StringHelpers::GetHash("sensor_2"), 0xac41a995u);
    zassert_equal(StringHelpers::GetHash("abc"), 0xad770999u);
}

ZTEST(string_helpers, test_GetHash_distinguishes_inputs) {
    zassert_not_equal(StringHelpers::GetHash("sensor_1"), StringHelpers::GetHash("sensor_2"));
    zassert_not_equal(StringHelpers::GetHash("abc"), StringHelpers::GetHash("cba"));
    zassert_not_equal(StringHelpers::GetHash("abc"), StringHelpers::GetHash("abc "));
}

ZTEST(string_helpers, test_GetHash_is_length_delimited) {
    // The view length, not a terminator, determines what is hashed.
    static constexpr char kPayload[] = "sensor_1_extra";

    zassert_equal(StringHelpers::GetHash(std::string_view(kPayload, 8)), StringHelpers::GetHash("sensor_1"));
}

ZTEST(string_helpers, test_UInt16ToStaticString) {
    zassert_str_equal(StringHelpers::UInt16ToStaticString(0).CStr(), "0");
    zassert_str_equal(StringHelpers::UInt16ToStaticString(7).CStr(), "7");
    zassert_str_equal(StringHelpers::UInt16ToStaticString(1234).CStr(), "1234");
    zassert_str_equal(StringHelpers::UInt16ToStaticString(UINT16_MAX).CStr(), "65535");
}

ZTEST(string_helpers, test_UInt16ToStaticString_size_and_capacity) {
    auto value = StringHelpers::UInt16ToStaticString(UINT16_MAX);

    zassert_equal(value.Size(), 5);
    zassert_equal(decltype(value)::Capacity(), 5);
    zassert_true(value == "65535");
}

ZTEST(string_helpers, test_UInt32ToStaticString) {
    zassert_str_equal(StringHelpers::UInt32ToStaticString(0).CStr(), "0");
    zassert_str_equal(StringHelpers::UInt32ToStaticString(65536).CStr(), "65536");
    zassert_str_equal(StringHelpers::UInt32ToStaticString(UINT32_MAX).CStr(), "4294967295");
}

ZTEST(string_helpers, test_UInt32ToStaticString_size_and_capacity) {
    auto value = StringHelpers::UInt32ToStaticString(UINT32_MAX);

    zassert_equal(value.Size(), 10);
    zassert_equal(decltype(value)::Capacity(), 10);
    zassert_true(value == "4294967295");
}
