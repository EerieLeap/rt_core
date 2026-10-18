#include <stdexcept>
#include <string>
#include <string_view>

#include <zephyr/ztest.h>

#include "domain/sensor_domain/models/sources/canbus_source.h"
#include "utilities/memory/memory_resource_manager.h"

using eerie_leap::domain::sensor_domain::models::sources::CanbusSource;
using eerie_leap::utilities::memory::Mrm;

ZTEST_SUITE(canbus_source, NULL, NULL, NULL, NULL, NULL);

namespace {

constexpr std::string_view FORMAT_ERROR =
    "Connection string must be bus_channel/frame_id or bus_channel/frame_id/signal_name.";
constexpr std::string_view BUS_ERROR = "Connection string bus channel must be a number from 0 to 255.";
constexpr std::string_view FRAME_ERROR = "Connection string frame ID must be a number from 0 to 0x1FFFFFFF.";

} // namespace

ZTEST(canbus_source, test_numbers_are_decimal_or_hex_after_0x) {
    const auto decimal = CanbusSource::Parse("010/0100/rpm");
    zassert_true(decimal.has_value());
    zassert_equal(decimal->bus_channel, 10, "A leading 0 is decimal, not octal");
    zassert_equal(decimal->frame_id, 100);
    zassert_true(decimal->signal_name == "rpm");

    const auto hex = CanbusSource::Parse("0x1/0X7e0");
    zassert_true(hex.has_value());
    zassert_equal(hex->bus_channel, 1);
    zassert_equal(hex->frame_id, 0x7E0);
    zassert_true(hex->signal_name.empty());
}

ZTEST(canbus_source, test_the_largest_bus_channel_and_frame_id_are_accepted) {
    const auto parts = CanbusSource::Parse("255/536870911");
    zassert_true(parts.has_value());
    zassert_equal(parts->bus_channel, 255);
    zassert_equal(parts->frame_id, CanbusSource::MAX_FRAME_ID);
}

ZTEST(canbus_source, test_the_signal_name_is_the_rest_of_the_text) {
    const auto parts = CanbusSource::Parse("0/1/a/b");
    zassert_true(parts.has_value());
    zassert_true(parts->signal_name == "a/b");
}

ZTEST(canbus_source, test_parse_says_what_is_wrong) {
    struct Case {
        const char* text;
        std::string_view error;
    };

    const Case cases[] = {
        { "", "Connection string cannot be empty." },
        { "1", FORMAT_ERROR },
        { "1/2/", FORMAT_ERROR },
        { "256/1", BUS_ERROR },
        { "-1/1", BUS_ERROR },
        { "+1/1", BUS_ERROR },
        { " 1/1", BUS_ERROR },
        { "1 /1", BUS_ERROR },
        { "0x/1", BUS_ERROR },
        { "/1", BUS_ERROR },
        { "1/536870912", FRAME_ERROR },
        { "1/0x20000000", FRAME_ERROR },
        { "1/4294967296", FRAME_ERROR },
        { "1/2x", FRAME_ERROR },
        { "1/", FRAME_ERROR },
        { "1//rpm", FRAME_ERROR },
    };

    for(const auto& test_case : cases) {
        const auto parts = CanbusSource::Parse(test_case.text);
        zassert_false(parts.has_value(), "Accepted '%s'", test_case.text);
        zassert_true(
            std::string_view(parts.error()) == test_case.error, "'%s' gave: %s", test_case.text, parts.error()
        );
    }
}

ZTEST(canbus_source, test_a_written_connection_string_reads_back) {
    const CanbusSource source(std::allocator_arg, Mrm::GetDefaultPmr(), 3, 0x1ABCDEF, "engine_speed");
    const auto text = source.ToConnectionString();
    zassert_true(text == "3/28036591/engine_speed", "Written in decimal: %s", text.c_str());

    const auto read = CanbusSource::FromConnectionString(Mrm::GetDefaultPmr(), text);
    zassert_equal(read.bus_channel, 3);
    zassert_equal(read.frame_id, 0x1ABCDEF);
    zassert_true(read.signal_name == "engine_speed");
    zassert_equal(read.signal_name_hash, source.signal_name_hash);
}

ZTEST(canbus_source, test_from_connection_string_throws_the_parse_error) {
    bool threw = false;

    try {
        CanbusSource::FromConnectionString(Mrm::GetDefaultPmr(), "1/2/");
    } catch(const std::invalid_argument& e) {
        threw = true;
        zassert_true(std::string_view(e.what()) == FORMAT_ERROR, "Threw: %s", e.what());
    }

    zassert_true(threw, "Accepted a connection string with an empty signal name");
}
