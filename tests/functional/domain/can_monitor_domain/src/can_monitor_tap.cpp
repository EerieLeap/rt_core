#include <algorithm>
#include <array>
#include <vector>

#include <zephyr/ztest.h>

#include "can_monitor_test_support.h"

using namespace can_monitor_test;

using eerie_leap::subsys::canbus::Canbus;
using eerie_leap::subsys::canbus::CanFrame;
using eerie_leap::subsys::canbus::CanId;

namespace {

constexpr uint32_t CONSUMER_ID = 0x123;
constexpr uint32_t OTHER_ID = 0x200;
// 47 bit times of overhead and 3 data bytes at 500 kbit/s: 71 x 2 µs.
constexpr uint64_t BUSY_US_PER_FRAME = 142;

// A consumer of can0: counts the frames its handler is given, from the receive thread.
class Consumer {
private:
    k_mutex lock_{};
    std::vector<CanFrame> frames_;

public:
    Consumer() {
        k_mutex_init(&lock_);
    }

    void Add(const CanFrame& frame) {
        k_mutex_lock(&lock_, K_FOREVER);
        frames_.push_back(frame);
        k_mutex_unlock(&lock_);
    }

    std::vector<CanFrame> Frames() {
        k_mutex_lock(&lock_, K_FOREVER);
        auto frames = frames_;
        k_mutex_unlock(&lock_);

        return frames;
    }
};

std::array<uint8_t, 3> PayloadOf(uint8_t index) {
    return { index, static_cast<uint8_t>(0xA0 + index), 0x55 };
}

void Send(Canbus& bus, uint32_t id, uint8_t index) {
    zassert_ok(bus.SendFrame(CanId::Standard(id), PayloadOf(index)));
}

// The frames of the stream so far with this identifier, received (tx false) or echoed (tx true).
size_t Count(Harness& harness, uint32_t id, bool is_tx) {
    size_t count = 0;
    for(const SentNotification& sent : harness.notifier->Notifications()) {
        const auto decoded = DecodeNotification(sent);
        zassert_true(decoded.has_value());

        for(const DecodedRecord& record : decoded->records) {
            if(record.record.kind == CanRecordKind::FRAME && record.record.id == id && record.record.tx == is_tx)
                ++count;
        }
    }

    return count;
}

} // namespace

ZTEST_SUITE(can_monitor_tap, NULL, NULL, NULL, ResetControllers, NULL);

ZTEST(can_monitor_tap, test_the_stream_carries_the_frames_and_echoes_and_the_consumers_are_left_alone) {
    Harness harness;
    Canbus& bus = *harness.Bus();

    Consumer consumer;
    const int handler_id = bus.RegisterFrameReceivedHandler(
        CanId::Standard(CONSUMER_ID), [&consumer](const CanFrame& frame) {
            consumer.Add(frame);
        }
    );
    zassert_true(handler_id > 0);

    // Before the tap: the consumer's frames, to compare with.
    for(uint8_t i = 0; i < 3; ++i)
        Send(bus, CONSUMER_ID, i);
    zassert_true(WaitUntil([&] {
        return consumer.Frames().size() == 3;
    }));

    CanMonitorOpenRequest request;
    request.ch = CONFIGURED_PORT;
    request.mode = CanMonitorMode::TAP;
    const auto answer = harness.service->Open(request);
    zassert_true(answer.has_value());
    zassert_equal(answer->ch, CONFIGURED_PORT);
    zassert_equal(answer->mode, CanMonitorMode::TAP);
    zassert_equal(answer->type, eerie_leap::subsys::canbus::CanbusType::CLASSICAL_CAN);
    zassert_equal(answer->bitrate, 500000U);
    zassert_equal(answer->data_bitrate, 0U);
    zassert_equal(answer->lease_ms, static_cast<uint32_t>(CONFIG_EERIE_LEAP_CAN_MONITOR_LEASE_MS));
    zassert_not_equal(answer->tok, 0U);
    zassert_equal(harness.holds.load(), 1, "The session holds the fast link");

    for(uint8_t i = 3; i < 8; ++i)
        Send(bus, CONSUMER_ID, i);

    // Each frame twice in the stream: as the bus received it (the loopback hands it back) and as the unit sent it.
    zassert_true(
        WaitUntil([&] {
            return Count(harness, CONSUMER_ID, false) == 5 && Count(harness, CONSUMER_ID, true) == 5;
        }),
        "Received %zu, sent %zu", Count(harness, CONSUMER_ID, false), Count(harness, CONSUMER_ID, true)
    );

    harness.CloseAndDrain(answer->tok);
    zassert_equal(harness.releases.load(), 1);

    // The consumer sees the frames after the tap goes as before it.
    for(uint8_t i = 8; i < 10; ++i)
        Send(bus, CONSUMER_ID, i);
    zassert_true(WaitUntil([&] {
        return consumer.Frames().size() >= 10;
    }));
    k_msleep(100);

    const auto frames = consumer.Frames();
    zassert_equal(frames.size(), 10U, "Every frame once, tap or not");
    for(size_t i = 0; i < frames.size(); ++i) {
        zassert_equal(frames[i].id, CONSUMER_ID);
        zassert_equal(frames[i].data.size(), 3U);
        const auto expected = PayloadOf(static_cast<uint8_t>(i));
        zassert_mem_equal(frames[i].data.data(), expected.data(), expected.size(), "Frame %zu", i);
        zassert_false(frames[i].is_transmit);
    }

    // The stream: the tap record first, the frames with their payloads, and nothing after CLOSE.
    const auto notifications = DecodeAll(harness.notifier->Notifications());
    ExpectChain(notifications);
    for(const DecodedNotification& notification : notifications)
        zassert_equal(notification.port, CONFIGURED_PORT);

    const auto taps = RecordsOf(notifications, CanRecordKind::TAP);
    zassert_equal(taps.size(), 1U);
    zassert_equal(taps[0]->record.tap_mark, CanTapMark::MAY_HAVE_REPEATED);

    std::vector<uint8_t> received;
    std::vector<uint8_t> sent;
    for(const DecodedRecord* frame : RecordsOf(notifications, CanRecordKind::FRAME)) {
        zassert_equal(frame->record.id, CONSUMER_ID);
        zassert_false(frame->record.ide || frame->record.fdf || frame->record.rtr || frame->record.esi);
        zassert_false(frame->record.tx_failed);
        zassert_equal(frame->payload.size(), 3U);
        (frame->record.tx ? sent : received).push_back(frame->payload[0]);
    }
    zassert_equal(received.size(), 5U);
    zassert_equal(sent.size(), 5U);
    for(uint8_t i = 0; i < 5; ++i) {
        zassert_equal(received[i], i + 3, "The bus sees the frames in the order they were sent");
        zassert_equal(sent[i], i + 3);
    }

    // The books: 5 frames each way, each 71 bit times at 2 µs.
    const CanStatistics sum = SumStatistics(notifications);
    zassert_equal(sum.frames_received, 5U);
    zassert_equal(sum.frames_sent, 5U);
    zassert_equal(sum.busy_us, 10 * BUSY_US_PER_FRAME);
    zassert_equal(sum.filtered, 0U);
    for(const uint64_t lost : sum.lost)
        zassert_equal(lost, 0U);

    zassert_true(bus.RemoveFrameReceivedHandler(handler_id));
}

ZTEST(can_monitor_tap, test_the_unit_filter_leaves_frames_out_and_counts_them) {
    Harness harness;
    Canbus& bus = *harness.Bus();

    const std::array<CanUnitFilter, 1> filters = { CanUnitFilter{ .id = OTHER_ID, .mask = 0x7FF, .ext = false } };
    CanMonitorOpenRequest request;
    request.ch = CONFIGURED_PORT;
    request.filters = filters;
    const auto answer = harness.service->Open(request);
    zassert_true(answer.has_value());

    for(uint8_t i = 0; i < 3; ++i)
        Send(bus, CONSUMER_ID, i);
    for(uint8_t i = 0; i < 3; ++i)
        Send(bus, OTHER_ID, i);

    zassert_true(WaitUntil([&] {
        return Count(harness, OTHER_ID, false) == 3 && Count(harness, OTHER_ID, true) == 3;
    }));
    k_msleep(100);
    harness.CloseAndDrain(answer->tok);

    const auto notifications = DecodeAll(harness.notifier->Notifications());
    for(const DecodedRecord* frame : RecordsOf(notifications, CanRecordKind::FRAME))
        zassert_equal(frame->record.id, OTHER_ID, "Only what the filter passes");

    // The bus carried 12 frame events; the statistics count all of them, and the filtered ones apart.
    const CanStatistics sum = SumStatistics(notifications);
    zassert_equal(sum.frames_received, 6U);
    zassert_equal(sum.frames_sent, 6U);
    zassert_equal(sum.filtered, 6U);
    zassert_equal(sum.busy_us, 12 * BUSY_US_PER_FRAME);
}

ZTEST(can_monitor_tap, test_a_change_of_state_is_a_record_with_the_counters) {
    Harness harness;

    const auto answer = harness.service->Open({ .ch = CONFIGURED_PORT });
    zassert_true(answer.has_value());

    harness.Bus()->OnBusStateChanged(CAN_STATE_ERROR_PASSIVE, { .tx_err_cnt = 130, .rx_err_cnt = 20 });

    zassert_true(WaitUntil([&] {
        const auto sent = harness.notifier->Notifications();

        return std::ranges::any_of(sent, [](const SentNotification& notification) {
            const auto decoded = DecodeNotification(notification);

            return decoded && !RecordsOf({ *decoded }, CanRecordKind::STATE).empty();
        });
    }));
    harness.CloseAndDrain(answer->tok);

    const auto notifications = DecodeAll(harness.notifier->Notifications());
    const auto states = RecordsOf(notifications, CanRecordKind::STATE);
    zassert_equal(states.size(), 1U);
    zassert_equal(states[0]->record.state, CanBusState::ERROR_PASSIVE);
    zassert_equal(states[0]->record.tec, 130);
    zassert_equal(states[0]->record.rec, 20);
    zassert_equal(states[0]->record.detected_bitrate, 0U, "The bitrate was configured, nothing was detected");
}
