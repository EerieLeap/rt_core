#include <string_view>
#include <zephyr/ztest.h>

#include "utilities/string/static_string.hpp"

using eerie_leap::utilities::string::StaticString;

ZTEST_SUITE(static_string, NULL, NULL, NULL, NULL, NULL);

ZTEST(static_string, test_default_construction) {
    StaticString<16> text;

    zassert_true(text.Empty());
    zassert_equal(text.Size(), 0);
    zassert_str_equal(text.CStr(), "");
    zassert_true(text.ToString().empty());
}

ZTEST(static_string, test_Capacity_is_compile_time) {
    static_assert(StaticString<16>::Capacity() == 16);

    zassert_equal(StaticString<16>::Capacity(), 16);
    zassert_equal(StaticString<3>::Capacity(), 3);
}

ZTEST(static_string, test_construction_from_string_view) {
    StaticString<16> text("sensor_1");

    zassert_false(text.Empty());
    zassert_equal(text.Size(), 8);
    zassert_str_equal(text.CStr(), "sensor_1");
    zassert_true(text.ToString() == "sensor_1");
}

ZTEST(static_string, test_construction_at_exact_capacity) {
    StaticString<8> text("sensor_1");

    zassert_equal(text.Size(), 8);
    zassert_str_equal(text.CStr(), "sensor_1");
}

ZTEST(static_string, test_Append_string_view) {
    StaticString<16> text("/lfs");

    zassert_true(text.Append("/config"));
    zassert_equal(text.Size(), 11);
    zassert_str_equal(text.CStr(), "/lfs/config");
}

ZTEST(static_string, test_Append_string_view_fills_capacity) {
    StaticString<4> text("ab");

    zassert_true(text.Append("cd"));
    zassert_str_equal(text.CStr(), "abcd");
}

ZTEST(static_string, test_Append_string_view_rejects_overflow) {
    StaticString<4> text("abc");

    zassert_false(text.Append("de"));
    zassert_equal(text.Size(), 3);
    zassert_str_equal(text.CStr(), "abc");

    zassert_true(text.Append("d"));
    zassert_str_equal(text.CStr(), "abcd");
}

ZTEST(static_string, test_Append_char) {
    StaticString<4> text;

    zassert_true(text.Append('a'));
    zassert_true(text.Append('b'));
    zassert_true(text.Append('c'));
    zassert_true(text.Append('d'));
    zassert_false(text.Append('e'));

    zassert_equal(text.Size(), 4);
    zassert_str_equal(text.CStr(), "abcd");
}

ZTEST(static_string, test_Append_empty_is_noop) {
    StaticString<8> text("abc");

    zassert_true(text.Append(""));
    zassert_equal(text.Size(), 3);
    zassert_str_equal(text.CStr(), "abc");
}

ZTEST(static_string, test_Truncate) {
    StaticString<16> text("/lfs/config");

    zassert_true(text.Truncate(4));
    zassert_equal(text.Size(), 4);
    zassert_str_equal(text.CStr(), "/lfs");
    zassert_true(text.ToString() == "/lfs");
}

ZTEST(static_string, test_Truncate_rejects_growth) {
    StaticString<16> text("abc");

    zassert_false(text.Truncate(4));
    zassert_equal(text.Size(), 3);
    zassert_str_equal(text.CStr(), "abc");
}

ZTEST(static_string, test_Truncate_then_Append) {
    StaticString<16> text("/lfs/config");

    text.Truncate(4);
    text.Append("/data");

    zassert_str_equal(text.CStr(), "/lfs/data");
}

ZTEST(static_string, test_Truncate_to_zero) {
    StaticString<16> text("abc");

    zassert_true(text.Truncate(0));
    zassert_true(text.Empty());
    zassert_str_equal(text.CStr(), "");
}

ZTEST(static_string, test_Clear) {
    StaticString<16> text("sensor_1");

    text.Clear();

    zassert_true(text.Empty());
    zassert_equal(text.Size(), 0);
    zassert_str_equal(text.CStr(), "");

    zassert_true(text.Append("again"));
    zassert_str_equal(text.CStr(), "again");
}

ZTEST(static_string, test_operator_plus_equal) {
    StaticString<16> text;

    text += "abc";
    text += 'd';
    text += "ef";

    zassert_equal(text.Size(), 6);
    zassert_str_equal(text.CStr(), "abcdef");
}

ZTEST(static_string, test_operator_plus_equal_silently_drops_overflow) {
    StaticString<4> text("abcd");

    text += "e";
    text += 'f';

    zassert_str_equal(text.CStr(), "abcd");
}

ZTEST(static_string, test_operator_index) {
    StaticString<16> text("abc");

    zassert_equal(text[0], 'a');
    zassert_equal(text[1], 'b');
    zassert_equal(text[2], 'c');
}

ZTEST(static_string, test_operator_equal_with_string_view) {
    StaticString<16> text("sensor_1");

    zassert_true(text == "sensor_1");
    zassert_false(text == "sensor_2");
    zassert_false(text == "sensor_1 ");
    zassert_false(text == "");
}

ZTEST(static_string, test_operator_equal_across_capacities) {
    StaticString<8> small("abc");
    StaticString<32> large("abc");
    StaticString<32> other("abcd");

    zassert_true(small == large);
    zassert_true(large == small);
    zassert_false(small == other);
}

ZTEST(static_string, test_ToString_tracks_size_not_buffer) {
    StaticString<16> text("abcdef");

    text.Truncate(3);

    zassert_equal(text.ToString().size(), 3);
    zassert_true(text.ToString() == "abc");
}
