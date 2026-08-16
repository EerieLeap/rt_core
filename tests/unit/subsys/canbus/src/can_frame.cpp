#include <cstdint>

#include <zephyr/ztest.h>

#include "subsys/canbus/can_frame.h"

using eerie_leap::subsys::canbus::CanFrame;

ZTEST_SUITE(can_frame, NULL, NULL, NULL, NULL, NULL);

ZTEST(can_frame, test_default_fields_are_zeroed) {
    CanFrame frame;

    zassert_equal(frame.id, 0);
    zassert_false(frame.is_extended);
    zassert_false(frame.is_transmit);
    zassert_false(frame.is_can_fd);
    zassert_false(frame.is_bitrate_switch);
    zassert_false(frame.is_remote_request);
    zassert_true(frame.data.empty());
}

ZTEST(can_frame, test_designated_initialization) {
    CanFrame frame {
        .id = 0x18FF1234,
        .is_extended = true,
        .is_can_fd = true,
        .data = {0x11, 0x22, 0x33}
    };

    zassert_equal(frame.id, 0x18FF1234);
    zassert_true(frame.is_extended);
    zassert_true(frame.is_can_fd);
    zassert_false(frame.is_bitrate_switch);
    zassert_equal(frame.data.size(), 3);
    zassert_equal(frame.data[2], 0x33);
}

ZTEST(can_frame, test_copy_is_independent) {
    CanFrame original {
        .id = 0x321,
        .data = {1, 2, 3}
    };

    CanFrame copy = original;
    copy.data[0] = 0xFF;

    zassert_equal(original.data[0], 1);
    zassert_equal(copy.data[0], 0xFF);
}
