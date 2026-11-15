#include <cerrno>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include <zephyr/drivers/can.h>
#include <zephyr/ztest.h>

#include "subsys/canbus/canbus_rx_entry.h"
#include "subsys/canbus/canbus_time.h"

using eerie_leap::subsys::canbus::CanbusRxEntry;
using eerie_leap::subsys::canbus::CanbusRxKind;
using eerie_leap::subsys::canbus::NowUs;

ZTEST_SUITE(canbus_rx_entry, NULL, NULL, NULL, NULL, NULL);

namespace {

can_frame MakeFrame() {
    can_frame frame{};
    frame.id = 0x18FF1234;
    frame.dlc = 3;
    frame.flags = CAN_FRAME_IDE | CAN_FRAME_FDF;
    frame.data[0] = 0x11;
    frame.data[1] = 0x22;
    frame.data[2] = 0x33;

    return frame;
}

} // namespace

ZTEST(canbus_rx_entry, test_entry_is_trivially_copyable) {
    // k_msgq copies the entry byte for byte.
    zassert_true(std::is_trivially_copyable_v<CanbusRxEntry>);
}

ZTEST(canbus_rx_entry, test_received_round_trips_its_fields) {
    const CanbusRxEntry entry = CanbusRxEntry::Received(MakeFrame(), 123456789012ULL);

    zassert_equal(entry.kind, CanbusRxKind::RECEIVED);
    zassert_equal(entry.time_us, 123456789012ULL);
    zassert_equal(entry.frame.id, 0x18FF1234);
    zassert_equal(entry.frame.dlc, 3);
    zassert_equal(entry.frame.flags, CAN_FRAME_IDE | CAN_FRAME_FDF);
    zassert_equal(entry.frame.data[2], 0x33);
    zassert_equal(entry.tx_error, 0);
    zassert_equal(entry.err_cnt.tx_err_cnt, 0);
    zassert_equal(entry.err_cnt.rx_err_cnt, 0);
}

ZTEST(canbus_rx_entry, test_tx_echo_round_trips_its_fields_and_error) {
    const CanbusRxEntry entry = CanbusRxEntry::TxEcho(MakeFrame(), 42, -ENETDOWN);

    zassert_equal(entry.kind, CanbusRxKind::TX_ECHO);
    zassert_equal(entry.time_us, 42);
    zassert_equal(entry.frame.id, 0x18FF1234);
    zassert_equal(entry.frame.data[0], 0x11);
    zassert_equal(entry.tx_error, -ENETDOWN);

    zassert_equal(CanbusRxEntry::TxEcho(MakeFrame(), 0, 0).tx_error, 0);
}

ZTEST(canbus_rx_entry, test_tx_error_is_clamped_to_int8) {
    zassert_equal(CanbusRxEntry::TxEcho(MakeFrame(), 0, -100000).tx_error, INT8_MIN);
    zassert_equal(CanbusRxEntry::TxEcho(MakeFrame(), 0, 100000).tx_error, INT8_MAX);
    zassert_equal(CanbusRxEntry::TxEcho(MakeFrame(), 0, -128).tx_error, -128);
    zassert_equal(CanbusRxEntry::TxEcho(MakeFrame(), 0, 127).tx_error, 127);
}

ZTEST(canbus_rx_entry, test_state_change_round_trips_its_fields) {
    const CanbusRxEntry entry = CanbusRxEntry::StateChange(
        CAN_STATE_ERROR_PASSIVE, { .tx_err_cnt = 130, .rx_err_cnt = 7 }, 99
    );

    zassert_equal(entry.kind, CanbusRxKind::STATE_CHANGE);
    zassert_equal(entry.state, CAN_STATE_ERROR_PASSIVE);
    zassert_equal(entry.err_cnt.tx_err_cnt, 130);
    zassert_equal(entry.err_cnt.rx_err_cnt, 7);
    zassert_equal(entry.time_us, 99);
    zassert_equal(entry.frame.id, 0, "A state entry carries no frame");
    zassert_equal(entry.tx_error, 0);
}

ZTEST(canbus_rx_entry, test_a_byte_copy_keeps_the_entry) {
    // What k_msgq_put() and k_msgq_get() do.
    const CanbusRxEntry original = CanbusRxEntry::TxEcho(MakeFrame(), 7, -5);
    CanbusRxEntry copy;
    std::memcpy(&copy, &original, sizeof(copy));

    zassert_equal(copy.kind, CanbusRxKind::TX_ECHO);
    zassert_equal(copy.time_us, 7);
    zassert_equal(copy.tx_error, -5);
    zassert_equal(copy.frame.id, original.frame.id);
    zassert_equal(copy.frame.data[1], 0x22);
}

ZTEST(canbus_rx_entry, test_default_entry_is_an_empty_received_frame) {
    const CanbusRxEntry entry;

    zassert_equal(entry.kind, CanbusRxKind::RECEIVED);
    zassert_equal(entry.time_us, 0);
    zassert_equal(entry.frame.id, 0);
    zassert_equal(entry.tx_error, 0);
}

ZTEST(canbus_rx_entry, test_now_us_does_not_go_back) {
    const uint64_t first = NowUs();
    k_sleep(K_MSEC(10));
    const uint64_t second = NowUs();

    zassert_true(second >= first);
    zassert_true(second - first < 10ULL * 1000 * 1000, "NowUs() must be in microseconds, not cycles");
}
