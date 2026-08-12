#include <chrono>
#include <cstdint>
#include <regex>
#include <string>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "subsys/time/time_helpers.hpp"

using namespace std::chrono;
using namespace eerie_leap::subsys::time;

namespace {

const std::regex kTimestampPattern(R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{1,3})");

} // namespace

ZTEST_SUITE(time_helpers, NULL, NULL, NULL, NULL, NULL);

ZTEST(time_helpers, test_GetFormattedString_matches_expected_pattern) {
    auto formatted_time = TimeHelpers::GetFormattedString(TimeHelpers::FromMilliseconds(1761106217000ms));

    zassert_not_equal(formatted_time.size(), 0);
    zassert_true(std::regex_match(formatted_time, kTimestampPattern), "unexpected format: %s", formatted_time.c_str());
}

ZTEST(time_helpers, test_GetFormattedString_appends_millisecond_remainder) {
    // The date part depends on the host time zone, so only the fraction is asserted verbatim.
    zassert_true(TimeHelpers::GetFormattedString(TimeHelpers::FromMilliseconds(1761106217000ms)).ends_with(".0"));
    zassert_true(TimeHelpers::GetFormattedString(TimeHelpers::FromMilliseconds(1761106217001ms)).ends_with(".1"));
    zassert_true(TimeHelpers::GetFormattedString(TimeHelpers::FromMilliseconds(1761106217999ms)).ends_with(".999"));
}

ZTEST(time_helpers, test_GetFormattedString_ignores_sub_millisecond_precision) {
    auto base = TimeHelpers::FromNanoseconds(1761106217123000000ns);
    auto skewed = TimeHelpers::FromNanoseconds(1761106217123999999ns);

    zassert_true(TimeHelpers::GetFormattedString(base) == TimeHelpers::GetFormattedString(skewed));
}

ZTEST(time_helpers, test_GetFormattedString_differs_for_different_days) {
    auto day = TimeHelpers::FromMilliseconds(1761106217000ms);
    auto next_day = TimeHelpers::FromMilliseconds(1761106217000ms + 24h);

    zassert_true(TimeHelpers::GetFormattedString(day) != TimeHelpers::GetFormattedString(next_day));
}

ZTEST(time_helpers, test_ToMilliseconds_returns_duration_since_epoch) {
    zassert_equal(TimeHelpers::ToMilliseconds(std::chrono::system_clock::time_point()).count(), 0);
    zassert_equal(TimeHelpers::ToMilliseconds(TimeHelpers::FromMilliseconds(1761106217000ms)).count(), 1761106217000);
}

ZTEST(time_helpers, test_ToMilliseconds_truncates_towards_epoch) {
    zassert_equal(TimeHelpers::ToMilliseconds(TimeHelpers::FromNanoseconds(1999999ns)).count(), 1);
    zassert_equal(TimeHelpers::ToMilliseconds(TimeHelpers::FromNanoseconds(999999ns)).count(), 0);
}

ZTEST(time_helpers, test_FromMilliseconds_round_trips) {
    for (auto value : {0ms, 1ms, 999ms, 1000ms, 1761106217000ms})
        zassert_equal(TimeHelpers::ToMilliseconds(TimeHelpers::FromMilliseconds(value)).count(), value.count());
}

ZTEST(time_helpers, test_ToNanoseconds_returns_duration_since_epoch) {
    zassert_equal(TimeHelpers::ToNanoseconds(std::chrono::system_clock::time_point()).count(), 0);
    zassert_equal(TimeHelpers::ToNanoseconds(TimeHelpers::FromMilliseconds(1234ms)).count(), 1234000000);
}

ZTEST(time_helpers, test_FromNanoseconds_round_trips) {
    for (auto value : {0ns, 1ns, 999ns, 1000ns, 1761106217123456789ns})
        zassert_equal(TimeHelpers::ToNanoseconds(TimeHelpers::FromNanoseconds(value)).count(), value.count());
}

ZTEST(time_helpers, test_millisecond_and_nanosecond_conversions_agree) {
    auto tp = TimeHelpers::FromMilliseconds(1761106217123ms);

    zassert_equal(TimeHelpers::ToNanoseconds(tp).count(), TimeHelpers::ToMilliseconds(tp).count() * 1000000);
}

ZTEST(time_helpers, test_ToUint32_returns_milliseconds_since_epoch) {
    zassert_equal(TimeHelpers::ToUint32(std::chrono::system_clock::time_point()), 0U);
    zassert_equal(TimeHelpers::ToUint32(TimeHelpers::FromMilliseconds(4294967295ms)), 4294967295U);
}

ZTEST(time_helpers, test_ToUint32_wraps_past_the_32_bit_range) {
    // 49.7 days worth of milliseconds is all a uint32 holds; callers get the wrapped remainder.
    zassert_equal(TimeHelpers::ToUint32(TimeHelpers::FromMilliseconds(4294967296ms)), 0U);
    zassert_equal(TimeHelpers::ToUint32(TimeHelpers::FromMilliseconds(4294967297ms)), 1U);
}

ZTEST(time_helpers, test_ToUint64_returns_nanoseconds_since_epoch) {
    zassert_equal(TimeHelpers::ToUint64(std::chrono::system_clock::time_point()), 0ULL);
    zassert_equal(TimeHelpers::ToUint64(TimeHelpers::FromMilliseconds(1761106217123ms)), 1761106217123000000ULL);
}

ZTEST(time_helpers, test_ToUint64_keeps_sub_millisecond_precision) {
    zassert_equal(TimeHelpers::ToUint64(TimeHelpers::FromNanoseconds(1761106217123456789ns)), 1761106217123456789ULL);
}

ZTEST(time_helpers, test_MeasureExecutionTimeUs_invokes_the_callable) {
    int calls = 0;

    TimeHelpers::MeasureExecutionTimeUs([&calls]() { ++calls; });

    zassert_equal(calls, 1);
}

ZTEST(time_helpers, test_MeasureExecutionTimeUs_returns_non_negative_duration) {
    auto elapsed_us = TimeHelpers::MeasureExecutionTimeUs([]() { });

    zassert_true(elapsed_us >= 0.0f, "measured %f us", (double)elapsed_us);
}

ZTEST(time_helpers, test_MeasureExecutionTimeUs_measures_busy_work) {
    auto elapsed_us = TimeHelpers::MeasureExecutionTimeUs([]() { k_busy_wait(5000); });

    // Emulated targets are noisy, so only the order of magnitude is checked.
    zassert_true(elapsed_us > 0.0f, "measured %f us", (double)elapsed_us);
    zassert_true(elapsed_us < 5000000.0f, "measured %f us", (double)elapsed_us);
}

ZTEST(time_helpers, test_MeasureExecutionTimeUs_can_be_called_repeatedly) {
    for (int i = 0; i < 3; ++i) {
        int calls = 0;

        auto elapsed_us = TimeHelpers::MeasureExecutionTimeUs([&calls]() { ++calls; });

        zassert_equal(calls, 1, "iteration %d", i);
        zassert_true(elapsed_us >= 0.0f, "iteration %d measured %f us", i, (double)elapsed_us);
    }
}
