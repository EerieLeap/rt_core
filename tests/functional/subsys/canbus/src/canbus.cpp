#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <zephyr/ztest.h>

#include "canbus_test_support.h"

using namespace canbus_test;

using eerie_leap::subsys::canbus::CanbusState;

// A failing assertion unwinds past the destructors, so the shared loopback
// controller has to be forced back to a stopped state for the next test.
static void ResetLoopbackDevice(void*) {
    can_stop(LoopbackDevice());
}

ZTEST_SUITE(canbus, NULL, NULL, ResetLoopbackDevice, NULL, NULL);

ZTEST(canbus, test_lifecycle_transitions) {
    Canbus canbus(MakeConfig());

    zassert_equal(canbus.GetState(), CanbusState::STOPPED);
    zassert_true(canbus.Initialize());
    zassert_equal(canbus.GetState(), CanbusState::STOPPED);

    zassert_true(canbus.Start());
    zassert_equal(canbus.GetState(), CanbusState::RUNNING);
    zassert_true(canbus.Start(), "Start() must be idempotent");

    zassert_true(canbus.Stop());
    zassert_equal(canbus.GetState(), CanbusState::STOPPED);
    zassert_true(canbus.Stop(), "Stop() must be idempotent");
}

ZTEST(canbus, test_start_requires_initialize) {
    Canbus canbus(MakeConfig());

    zassert_false(canbus.Start(), "Start() must fail before Initialize()");
    zassert_equal(canbus.GetState(), CanbusState::STOPPED);
}

ZTEST(canbus, test_unsupported_bitrate_is_rejected) {
    Canbus canbus(MakeConfig(CanbusType::CLASSICAL_CAN, 500000));

    zassert_false(canbus.Configure(MakeConfig(CanbusType::CLASSICAL_CAN, 123456)));
}

ZTEST(canbus, test_configure_rolls_back_on_failure) {
    Canbus canbus(MakeConfig(CanbusType::CLASSICAL_CAN, 500000));
    zassert_true(canbus.Initialize());

    zassert_false(canbus.Configure(MakeConfig(CanbusType::CLASSICAL_CAN, 7)));
    zassert_equal(canbus.GetDetectedBitrate(), 500000, "Failed Configure() must not clobber the live config");
}

ZTEST(canbus, test_register_handler_returns_positive_id) {
    auto canbus = MakeRunningCanbus();
    FrameCollector collector;

    int first = canbus->RegisterFrameReceivedHandler(0x100, [&](const CanFrame& f) { collector.Collect(f); });
    int second = canbus->RegisterFrameReceivedHandler(0x100, [&](const CanFrame& f) { collector.Collect(f); });

    zassert_true(first > 0, "Handler ids must be strictly positive");
    zassert_true(second > 0);
    zassert_not_equal(first, second, "Handler ids must be unique");

    zassert_true(canbus->RemoveFrameReceivedHandler(0x100, first));
    zassert_true(canbus->RemoveFrameReceivedHandler(0x100, second));
}

ZTEST(canbus, test_register_handler_before_initialize_fails) {
    Canbus canbus(MakeConfig());

    int result = canbus.RegisterFrameReceivedHandler(0x100, [](const CanFrame&) {});

    zassert_equal(result, Canbus::ERR_NOT_INITIALIZED);
}

ZTEST(canbus, test_register_empty_handler_fails) {
    auto canbus = MakeRunningCanbus();

    int result = canbus->RegisterFrameReceivedHandler(0x100, nullptr);

    zassert_equal(result, Canbus::ERR_INVALID_ARGUMENT);
}

ZTEST(canbus, test_register_handler_rejects_id_wider_than_configured) {
    auto canbus = MakeRunningCanbus(CanbusType::CLASSICAL_CAN, 500000, false);

    int result = canbus->RegisterFrameReceivedHandler(0x18FF1234, [](const CanFrame&) {});

    zassert_equal(result, Canbus::ERR_INVALID_ARGUMENT,
        "A 29-bit id must be rejected on a standard-id bus");
}

ZTEST(canbus, test_handler_limit_is_enforced) {
    auto canbus = MakeRunningCanbus();

    std::vector<int> ids;
    for(int i = 0; i < 8; i++) {
        int id = canbus->RegisterFrameReceivedHandler(0x200, [](const CanFrame&) {});
        zassert_true(id > 0, "Handler %d should have been accepted", i);
        ids.push_back(id);
    }

    zassert_equal(
        canbus->RegisterFrameReceivedHandler(0x200, [](const CanFrame&) {}),
        Canbus::ERR_TOO_MANY_HANDLERS);

    for(int id : ids)
        zassert_true(canbus->RemoveFrameReceivedHandler(0x200, id));
}

ZTEST(canbus, test_remove_handler_contract) {
    auto canbus = MakeRunningCanbus();

    int id = canbus->RegisterFrameReceivedHandler(0x300, [](const CanFrame&) {});
    zassert_true(id > 0);

    zassert_false(canbus->RemoveFrameReceivedHandler(0x300, id + 100), "Unknown handler id must fail");
    zassert_false(canbus->RemoveFrameReceivedHandler(0x301, id), "Unknown can id must fail");
    zassert_true(canbus->RemoveFrameReceivedHandler(0x300, id));
    zassert_false(canbus->RemoveFrameReceivedHandler(0x300, id), "Double removal must fail");
}

ZTEST(canbus, test_send_and_receive_round_trip) {
    auto canbus = MakeRunningCanbus();
    FrameCollector collector;

    int id = canbus->RegisterFrameReceivedHandler(0x321, [&](const CanFrame& f) { collector.Collect(f); });
    zassert_true(id > 0);

    std::array<uint8_t, 8> payload = {1, 2, 3, 4, 5, 6, 7, 8};
    zassert_true(canbus->SendFrame(0x321, payload));

    zassert_true(collector.Wait(), "Frame was not delivered");
    zassert_equal(collector.Count(), 1);

    CanFrame received = collector.At(0);
    zassert_equal(received.id, 0x321);
    zassert_false(received.is_extended);
    zassert_equal(received.data.size(), 8);
    for(size_t i = 0; i < payload.size(); i++)
        zassert_equal(received.data[i], payload[i]);
}

ZTEST(canbus, test_extended_id_round_trip) {
    auto canbus = MakeRunningCanbus(CanbusType::CLASSICAL_CAN, 500000, true);
    FrameCollector collector;

    constexpr uint32_t extended_id = 0x18FF1234;

    int id = canbus->RegisterFrameReceivedHandler(extended_id, [&](const CanFrame& f) { collector.Collect(f); });
    zassert_true(id > 0, "Extended ids must be accepted on an extended-id bus");

    std::array<uint8_t, 4> payload = {0xDE, 0xAD, 0xBE, 0xEF};
    zassert_true(canbus->SendFrame(extended_id, payload));

    zassert_true(collector.Wait(), "Extended frame was not delivered");

    CanFrame received = collector.At(0);
    zassert_equal(received.id, extended_id);
    zassert_true(received.is_extended);
    zassert_equal(received.data.size(), 4);
    zassert_equal(received.data[0], 0xDE);
}

ZTEST(canbus, test_extended_id_filter_does_not_alias_lower_bits) {
    auto canbus = MakeRunningCanbus(CanbusType::CLASSICAL_CAN, 500000, true);
    FrameCollector collector;

    // 0x18FF1234 and 0x1CFF1234 share their low 11 bits.
    int id = canbus->RegisterFrameReceivedHandler(0x18FF1234, [&](const CanFrame& f) { collector.Collect(f); });
    zassert_true(id > 0);

    std::array<uint8_t, 1> payload = {0x01};
    zassert_true(canbus->SendFrame(0x1CFF1234, payload));

    zassert_false(collector.Wait(200), "A different extended id must not match the filter");
}

ZTEST(canbus, test_oversized_payload_is_rejected) {
    auto canbus = MakeRunningCanbus(CanbusType::CLASSICAL_CAN);

    std::vector<uint8_t> payload(64, 0xAA);

    zassert_false(canbus->SendFrame(0x100, payload),
        "Classical CAN must reject payloads longer than 8 bytes");
}

ZTEST(canbus, test_huge_payload_is_rejected) {
    auto canbus = MakeRunningCanbus(CanbusType::CANFD);

    std::vector<uint8_t> payload(512, 0xAA);

    zassert_false(canbus->SendFrame(0x100, payload),
        "Payloads beyond the CAN FD limit must be rejected");
}

ZTEST(canbus, test_send_rejects_id_wider_than_configured) {
    auto canbus = MakeRunningCanbus(CanbusType::CLASSICAL_CAN, 500000, false);

    std::array<uint8_t, 1> payload = {0x01};

    zassert_false(canbus->SendFrame(0x18FF1234, payload),
        "A 29-bit id must be rejected on a standard-id bus");
}

ZTEST(canbus, test_send_before_start_is_rejected) {
    Canbus canbus(MakeConfig());
    zassert_true(canbus.Initialize());

    std::array<uint8_t, 1> payload = {0x01};

    zassert_false(canbus.SendFrame(0x100, payload), "SendFrame() must fail while not RUNNING");
}

ZTEST(canbus, test_canfd_payload_round_trip) {
    auto canbus = MakeRunningCanbus(CanbusType::CANFD, 500000);
    FrameCollector collector;

    int id = canbus->RegisterFrameReceivedHandler(0x123, [&](const CanFrame& f) { collector.Collect(f); });
    zassert_true(id > 0);

    std::vector<uint8_t> payload(32, 0x77);
    zassert_true(canbus->SendFrame(0x123, payload));

    zassert_true(collector.Wait(), "CAN FD frame was not delivered");

    CanFrame received = collector.At(0);
    zassert_true(received.is_can_fd);
    zassert_equal(received.data.size(), 32);
    zassert_equal(received.data[31], 0x77);
}

ZTEST(canbus, test_all_handlers_for_an_id_are_invoked) {
    auto canbus = MakeRunningCanbus();
    FrameCollector first;
    FrameCollector second;

    zassert_true(canbus->RegisterFrameReceivedHandler(0x400, [&](const CanFrame& f) { first.Collect(f); }) > 0);
    zassert_true(canbus->RegisterFrameReceivedHandler(0x400, [&](const CanFrame& f) { second.Collect(f); }) > 0);

    std::array<uint8_t, 2> payload = {0x10, 0x20};
    zassert_true(canbus->SendFrame(0x400, payload));

    zassert_true(first.Wait(), "First handler was not invoked");
    zassert_true(second.Wait(), "Second handler was not invoked");
}

ZTEST(canbus, test_removed_handler_stops_receiving) {
    auto canbus = MakeRunningCanbus();
    FrameCollector collector;

    int id = canbus->RegisterFrameReceivedHandler(0x500, [&](const CanFrame& f) { collector.Collect(f); });
    zassert_true(id > 0);
    zassert_true(canbus->RemoveFrameReceivedHandler(0x500, id));

    std::array<uint8_t, 1> payload = {0x01};
    zassert_true(canbus->SendFrame(0x500, payload));

    zassert_false(collector.Wait(200), "A removed handler must not be invoked");
}

ZTEST(canbus, test_handler_may_unregister_itself_during_dispatch) {
    auto canbus = MakeRunningCanbus();
    FrameCollector collector;

    int id = 0;
    id = canbus->RegisterFrameReceivedHandler(0x600, [&](const CanFrame& frame) {
        collector.Collect(frame);
        canbus->RemoveFrameReceivedHandler(0x600, id);
    });
    zassert_true(id > 0);

    std::array<uint8_t, 1> payload = {0x01};
    zassert_true(canbus->SendFrame(0x600, payload));
    zassert_true(collector.Wait());

    zassert_true(canbus->SendFrame(0x600, payload));
    zassert_false(collector.Wait(200), "Handler should have removed itself");
}

ZTEST(canbus, test_unfiltered_ids_are_not_delivered) {
    auto canbus = MakeRunningCanbus();
    FrameCollector collector;

    zassert_true(canbus->RegisterFrameReceivedHandler(0x700, [&](const CanFrame& f) { collector.Collect(f); }) > 0);

    std::array<uint8_t, 1> payload = {0x01};
    zassert_true(canbus->SendFrame(0x701, payload));

    zassert_false(collector.Wait(200), "Only filtered ids may be delivered");
}

ZTEST(canbus, test_stop_clears_handlers_and_rx_state) {
    auto canbus = MakeRunningCanbus();
    FrameCollector collector;

    int id = canbus->RegisterFrameReceivedHandler(0x108, [&](const CanFrame& f) { collector.Collect(f); });
    zassert_true(id > 0);

    zassert_true(canbus->Stop());

    zassert_false(canbus->IsBitrateDetected());
    zassert_equal(
        canbus->RegisterFrameReceivedHandler(0x108, [](const CanFrame&) {}),
        Canbus::ERR_NOT_INITIALIZED,
        "Stop() must leave the bus uninitialized");
}

ZTEST(canbus, test_reconfigure_after_stop) {
    Canbus canbus(MakeConfig(CanbusType::CLASSICAL_CAN, 500000));

    zassert_true(canbus.Initialize());
    zassert_true(canbus.Start());
    zassert_true(canbus.Stop());

    zassert_true(canbus.Configure(MakeConfig(CanbusType::CLASSICAL_CAN, 250000)));
    zassert_true(canbus.Start());
    zassert_equal(canbus.GetDetectedBitrate(), 250000);

    FrameCollector collector;
    zassert_true(canbus.RegisterFrameReceivedHandler(0x109, [&](const CanFrame& f) { collector.Collect(f); }) > 0);

    std::array<uint8_t, 1> payload = {0x42};
    zassert_true(canbus.SendFrame(0x109, payload));
    zassert_true(collector.Wait(), "Bus must work again after a reconfigure");

    zassert_true(canbus.Stop());
}

ZTEST(canbus, test_rx_drop_counter_starts_at_zero) {
    auto canbus = MakeRunningCanbus();

    zassert_equal(canbus->GetRxDroppedCount(), 0);
}

ZTEST(canbus, test_auto_detect_defers_filters_but_returns_valid_handler_id) {
    // Bitrate 0 selects auto detection; the loopback bus is silent so it never
    // completes, which is exactly the window where handler ids used to be lost.
    auto canbus = std::make_unique<Canbus>(MakeConfig(CanbusType::CLASSICAL_CAN, 0));

    zassert_true(canbus->Initialize());
    zassert_true(canbus->Start());
    zassert_false(canbus->IsBitrateDetected());

    int id = canbus->RegisterFrameReceivedHandler(0x10A, [](const CanFrame&) {});
    zassert_true(id > 0, "A deferred handler must still return a usable id, got %d", id);
    zassert_true(canbus->RemoveFrameReceivedHandler(0x10A, id),
        "The id returned during auto-detect must be removable");

    zassert_true(canbus->Stop());
}

ZTEST(canbus, test_destruction_while_running_is_safe) {
    FrameCollector collector;

    {
        auto canbus = MakeRunningCanbus();
        zassert_true(canbus->RegisterFrameReceivedHandler(0x10B, [&](const CanFrame& f) { collector.Collect(f); }) > 0);

        std::array<uint8_t, 1> payload = {0x01};
        zassert_true(canbus->SendFrame(0x10B, payload));
        zassert_true(collector.Wait());
    }

    // The filter must be gone with the object; a fresh instance on the same
    // device proves the driver is not still holding the destroyed callback.
    auto replacement = MakeRunningCanbus();
    FrameCollector replacement_collector;

    zassert_true(replacement->RegisterFrameReceivedHandler(
        0x10B, [&](const CanFrame& f) { replacement_collector.Collect(f); }) > 0);

    std::array<uint8_t, 1> payload = {0x02};
    zassert_true(replacement->SendFrame(0x10B, payload));

    zassert_true(replacement_collector.Wait());
    zassert_equal(collector.Count(), 1, "The destroyed instance must not receive anything");
}
