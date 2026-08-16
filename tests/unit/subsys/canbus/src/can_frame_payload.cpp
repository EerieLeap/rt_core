#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <zephyr/ztest.h>

#include "subsys/canbus/can_frame_payload.h"

using eerie_leap::subsys::canbus::CanFramePayload;
using eerie_leap::subsys::canbus::CAN_FRAME_MAX_DATA_LENGTH;

ZTEST_SUITE(can_frame_payload, NULL, NULL, NULL, NULL, NULL);

ZTEST(can_frame_payload, test_default_payload_is_empty) {
    CanFramePayload payload;

    zassert_equal(payload.size(), 0);
    zassert_true(payload.empty());
    zassert_equal(payload.capacity(), CAN_FRAME_MAX_DATA_LENGTH);
}

ZTEST(can_frame_payload, test_initializer_list_construction) {
    CanFramePayload payload({0x11, 0x22, 0x33});

    zassert_equal(payload.size(), 3);
    zassert_equal(payload[0], 0x11);
    zassert_equal(payload[2], 0x33);
}

ZTEST(can_frame_payload, test_vector_construction) {
    std::vector<uint8_t> bytes(8, 0x5A);
    CanFramePayload payload(bytes);

    zassert_equal(payload.size(), 8);
    for(size_t i = 0; i < payload.size(); i++)
        zassert_equal(payload[i], 0x5A);
}

ZTEST(can_frame_payload, test_array_construction) {
    std::array<uint8_t, 4> bytes = {1, 2, 3, 4};
    CanFramePayload payload(bytes);

    zassert_equal(payload.size(), 4);
    zassert_equal(payload[3], 4);
}

ZTEST(can_frame_payload, test_assign_replaces_content) {
    CanFramePayload payload({1, 2, 3, 4, 5, 6, 7, 8});

    std::array<uint8_t, 2> shorter = {0xAA, 0xBB};
    payload.Assign(shorter);

    zassert_equal(payload.size(), 2);
    zassert_equal(payload[0], 0xAA);
    zassert_equal(payload[1], 0xBB);
    // Stale bytes from the previous, longer content must not leak through.
    zassert_equal(payload.data()[2], 0);
    zassert_equal(payload.data()[7], 0);
}

ZTEST(can_frame_payload, test_oversized_input_is_truncated_not_overflowing) {
    std::vector<uint8_t> bytes(CAN_FRAME_MAX_DATA_LENGTH * 4, 0xEE);
    CanFramePayload payload(bytes);

    zassert_equal(payload.size(), CAN_FRAME_MAX_DATA_LENGTH);
    zassert_equal(payload[CAN_FRAME_MAX_DATA_LENGTH - 1], 0xEE);
}

ZTEST(can_frame_payload, test_resize_shrinks_and_clears) {
    CanFramePayload payload({1, 2, 3, 4});
    payload.Resize(2);

    zassert_equal(payload.size(), 2);
    zassert_equal(payload.data()[2], 0);

    payload.Clear();
    zassert_true(payload.empty());
}

ZTEST(can_frame_payload, test_resize_is_clamped_to_capacity) {
    CanFramePayload payload;
    payload.Resize(CAN_FRAME_MAX_DATA_LENGTH * 2);

    zassert_equal(payload.size(), CAN_FRAME_MAX_DATA_LENGTH);
}

ZTEST(can_frame_payload, test_span_conversion_matches_size) {
    CanFramePayload payload({9, 8, 7});
    std::span<const uint8_t> view = payload;

    zassert_equal(view.size(), 3);
    zassert_equal(view[0], 9);
    zassert_equal(view[2], 7);
}

ZTEST(can_frame_payload, test_iteration_stops_at_size) {
    CanFramePayload payload({4, 4, 4});

    size_t count = 0;
    for(uint8_t byte : payload) {
        zassert_equal(byte, 4);
        count++;
    }

    zassert_equal(count, 3);
}

ZTEST(can_frame_payload, test_equality) {
    CanFramePayload left({1, 2, 3});
    CanFramePayload right({1, 2, 3});
    CanFramePayload shorter({1, 2});

    zassert_true(left == right);
    zassert_false(left == shorter);
}

ZTEST(can_frame_payload, test_copy_is_independent) {
    CanFramePayload original({1, 2, 3});
    CanFramePayload copy = original;

    copy[0] = 0xFF;

    zassert_equal(original[0], 1);
    zassert_equal(copy[0], 0xFF);
}
