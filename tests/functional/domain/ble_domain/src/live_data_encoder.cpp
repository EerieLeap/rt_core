#include <array>
#include <optional>

#include <zephyr/ztest.h>

#include "live_data_test_support.h"

using namespace live_data_test;

ZTEST_SUITE(live_data_encoder, NULL, NULL, NULL, NULL, NULL);

ZTEST(live_data_encoder, test_layout_leaves_out_sensors_without_a_value) {
    const std::array<std::optional<float>, 3> values = {1.5F, std::nullopt, -2.0F};
    std::array<uint8_t, LiveDataEncoder::GetMaxSize(3)> out{};

    const size_t size = LiveDataEncoder::Encode(7, 0x01020304, values, out);

    const std::array<uint8_t, 17> expected = {
        0x01, 0x07, 0x04, 0x03, 0x02, 0x01, 0x02,
        0x00, 0x00, 0x00, 0xC0, 0x3F,
        0x02, 0x00, 0x00, 0x00, 0xC0,
    };
    zassert_equal(size, expected.size());
    zassert_mem_equal(out.data(), expected.data(), expected.size());
}

ZTEST(live_data_encoder, test_no_values_leaves_the_header) {
    const std::array<std::optional<float>, 2> values = {};
    std::array<uint8_t, LiveDataEncoder::GetMaxSize(2)> out{};

    zassert_equal(LiveDataEncoder::Encode(1, 2, values, out), LiveDataEncoder::HEADER_SIZE);
    zassert_equal(out[6], 0);
}

ZTEST(live_data_encoder, test_rejects_a_short_buffer_and_too_many_values) {
    const std::array<std::optional<float>, 2> values = {1.0F, 2.0F};
    std::array<uint8_t, LiveDataEncoder::GetMaxSize(2) - 1> short_out{};
    zassert_equal(LiveDataEncoder::Encode(0, 0, values, short_out), 0);

    std::vector<std::optional<float>> many(LiveDataEncoder::MAX_VALUES + 1);
    std::vector<uint8_t> out(LiveDataEncoder::GetMaxSize(many.size()));
    zassert_equal(LiveDataEncoder::Encode(0, 0, many, out), 0);
}

ZTEST(live_data_encoder, test_a_full_subscription_fills_one_notification) {
    // ATT MTU 247 leaves 244 bytes.
    zassert_true(LiveDataEncoder::GetMaxSize(LiveDataService::MAX_SENSORS) <= 244);
    zassert_true(LiveDataEncoder::GetMaxSize(LiveDataService::MAX_SENSORS + 1) > 244);
}
