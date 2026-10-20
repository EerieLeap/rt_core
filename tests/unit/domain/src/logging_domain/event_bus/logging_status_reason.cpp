#include <cstdint>
#include <string_view>

#include <zephyr/ztest.h>

#include "domain/logging_domain/event_bus/logging_status_reason.h"

using eerie_leap::domain::logging_domain::event_bus::GetReasonName;
using eerie_leap::domain::logging_domain::event_bus::LoggingStatusReason;

// Usable where a constant is needed, such as a static table of log text.
static_assert(std::string_view(GetReasonName(LoggingStatusReason::Stopped)) == "stopped");

ZTEST_SUITE(logging_status_reason, NULL, NULL, NULL, NULL, NULL);

ZTEST(logging_status_reason, test_every_reason_has_its_name) {
    zassert_str_equal(GetReasonName(LoggingStatusReason::Stopped), "stopped");
    zassert_str_equal(GetReasonName(LoggingStatusReason::WriteFailed), "write failed");
    zassert_str_equal(GetReasonName(LoggingStatusReason::NoSpace), "no space");
    zassert_str_equal(GetReasonName(LoggingStatusReason::RotationFailed), "rotation failed");
    zassert_str_equal(GetReasonName(LoggingStatusReason::Unavailable), "unavailable");
    zassert_str_equal(GetReasonName(LoggingStatusReason::FirmwareUpdate), "firmware update");
}

// The values travel in the CDMP record and over SMP: a new one is appended, no value moves.
ZTEST(logging_status_reason, test_the_values_are_stable) {
    zassert_equal(static_cast<uint32_t>(LoggingStatusReason::Stopped), 0U);
    zassert_equal(static_cast<uint32_t>(LoggingStatusReason::WriteFailed), 1U);
    zassert_equal(static_cast<uint32_t>(LoggingStatusReason::NoSpace), 2U);
    zassert_equal(static_cast<uint32_t>(LoggingStatusReason::RotationFailed), 3U);
    zassert_equal(static_cast<uint32_t>(LoggingStatusReason::Unavailable), 4U);
    zassert_equal(static_cast<uint32_t>(LoggingStatusReason::FirmwareUpdate), 5U);
}

// Another unit's record carries the byte as it is, so a newer unit can send a value this build lacks.
ZTEST(logging_status_reason, test_an_unknown_reason_is_named_unknown) {
    zassert_str_equal(GetReasonName(static_cast<LoggingStatusReason>(200)), "unknown");
    zassert_str_equal(GetReasonName(static_cast<LoggingStatusReason>(UINT32_MAX)), "unknown");
}
