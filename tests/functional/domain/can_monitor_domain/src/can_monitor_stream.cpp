#include <cerrno>

#include <zephyr/ztest.h>

#include "can_monitor_test_support.h"

using namespace can_monitor_test;

namespace {

// A failing assertion skips the harness, so the controller is stopped separately.
void ResetControllers(void*) {
    can_stop(ConfiguredDevice());
}

/// The frames of the stream, in order.
std::vector<const DecodedRecord*> FramesOf(const std::vector<DecodedNotification>& notifications) {
    return RecordsOf(notifications, CanRecordKind::FRAME);
}

} // namespace

ZTEST_SUITE(can_monitor_stream, NULL, NULL, NULL, ResetControllers, NULL);

ZTEST(can_monitor_stream, test_notifications_hold_whole_records_and_chain_back_to_the_frames) {
    Harness harness;
    constexpr uint32_t FPS = 2000;
    constexpr uint32_t IDS = 4;

    const uint64_t before_us = NowUs();
    const auto answer = harness.OpenSynthetic(FPS, IDS);
    const uint64_t after_us = NowUs();

    k_msleep(400);
    harness.CloseAndDrain(answer.tok);

    const auto sent = harness.notifier->Notifications();
    zassert_true(sent.size() > 2, "A stream of several notifications");
    for(const SentNotification& notification : sent) {
        zassert_true(notification.data.size() <= NOTIFY_MAX, "A notification of %zu bytes", notification.data.size());
        zassert_equal(notification.data[1], CAN_SYNTHETIC_PORT);
    }

    // The first `base_us` is the OPEN's time.
    const uint64_t first_base_us = sys_get_le64(&sent[0].data[4]);
    zassert_true(first_base_us >= before_us && first_base_us <= after_us);

    // Every frame of the generator, in order, at a time that never goes back; seq and base_us chain (WalkStream).
    size_t frames = 0;
    uint64_t last_time_us = first_base_us;
    WalkStream(sent, [&](const CanRecord& record, uint64_t time_us) {
        zassert_true(time_us >= last_time_us);
        last_time_us = time_us;

        if(record.kind != CanRecordKind::FRAME)
            return;

        zassert_equal(record.id, static_cast<uint32_t>(frames % IDS));
        zassert_equal(record.dlc, 8);
        zassert_false(record.ide || record.fdf || record.brs || record.rtr || record.tx);
        zassert_equal(record.payload.size(), 8U);
        for(size_t k = 0; k < 8; ++k)
            zassert_equal(
                record.payload[k], CanSyntheticSource::PayloadByte(frames, k), "Byte %zu of frame %zu", k, frames
            );

        ++frames;
    });

    zassert_true(frames > 300, "Frames came: %zu", frames);
    // The last frame was generated before CLOSE returned.
    zassert_true(last_time_us <= NowUs());
}

ZTEST(can_monitor_stream, test_at_most_four_notifications_wait_for_their_completion) {
    Harness harness;
    harness.is_auto_complete = false;

    const auto answer = harness.OpenSynthetic(5000, 8);

    zassert_true(WaitUntil([&] {
        return harness.notifier->Count() >= CONFIG_EERIE_LEAP_CAN_MONITOR_IN_FLIGHT;
    }));
    k_msleep(300);

    zassert_equal(harness.notifier->Count(), CONFIG_EERIE_LEAP_CAN_MONITOR_IN_FLIGHT, "No fifth one goes");
    zassert_equal(harness.service->GetInFlight(), CONFIG_EERIE_LEAP_CAN_MONITOR_IN_FLIGHT);

    // A full notification waits for nothing: these left because they filled up.
    for(const SentNotification& sent : harness.notifier->Notifications()) {
        zassert_true(sent.data.size() <= NOTIFY_MAX);
        zassert_true(sent.data.size() + CAN_MAX_RECORD_SIZE > NOTIFY_MAX, "Full: %zu bytes", sent.data.size());
    }

    // One completion lets exactly one more go.
    harness.service->OnNotificationSent();
    zassert_true(WaitUntil([&] {
        return harness.notifier->Count() == CONFIG_EERIE_LEAP_CAN_MONITOR_IN_FLIGHT + 1;
    }));
    k_msleep(100);
    zassert_equal(harness.notifier->Count(), CONFIG_EERIE_LEAP_CAN_MONITOR_IN_FLIGHT + 1);

    // The ring was overrun meanwhile; the stream still chains.
    harness.CloseAndDrain(answer.tok);
    const StreamSummary summary = Summarize(harness.notifier->Notifications());
    zassert_true(summary.gaps > 0);
    zassert_true(summary.largest_notification <= NOTIFY_MAX);
}

ZTEST(can_monitor_stream, test_a_lone_record_leaves_after_the_flush_time) {
    Harness harness;

    const int64_t opened_ms = k_uptime_get();
    const auto answer = harness.OpenSynthetic(1, 1, 2);

    zassert_true(WaitUntil([&] {
        return harness.notifier->Count() >= 1;
    }));

    const auto notifications = DecodeAll(harness.notifier->Notifications());
    const int64_t waited_ms = notifications[0].time_ms - opened_ms;

    // Held for the flush time, give or take the clock and the scheduler; QEMU is slow, so the upper bound is wide.
    zassert_true(waited_ms >= static_cast<int64_t>(FLUSH_MS) - 10, "Left after %d ms", static_cast<int>(waited_ms));
    zassert_true(waited_ms <= static_cast<int64_t>(FLUSH_MS) + 500, "Left after %d ms", static_cast<int>(waited_ms));
    zassert_equal(FramesOf(notifications).size(), 1);
    zassert_equal(notifications[0].sequence, 0);

    harness.CloseAndDrain(answer.tok);
}

ZTEST(can_monitor_stream, test_statistics_come_each_second_and_add_up) {
    Harness harness;

    const auto answer = harness.OpenSynthetic(100, 2, 4);
    k_msleep(2600);
    harness.CloseAndDrain(answer.tok);

    const auto notifications = DecodeAll(harness.notifier->Notifications());
    ExpectChain(notifications);

    const auto statistics = RecordsOf(notifications, CanRecordKind::STATISTICS);
    // One at about 1 s, one at about 2 s, and the final one.
    zassert_true(statistics.size() >= 3, "Statistics records: %zu", statistics.size());

    // The first full second holds about a second of frames.
    const uint64_t first = statistics[0]->record.statistics.frames_received;
    zassert_true(first >= 50 && first <= 150, "Frames in the first second: %u", static_cast<unsigned>(first));

    // Sender records take no time of their own.
    for(const DecodedNotification& notification : notifications) {
        for(size_t i = 0; i < notification.records.size(); ++i) {
            if(notification.records[i].record.kind != CanRecordKind::STATISTICS)
                continue;

            const uint64_t before = i > 0 ? notification.records[i - 1].time_us : notification.base_us;
            zassert_equal(notification.records[i].time_us, before);
        }
    }

    // What the records add up to is what the stream holds.
    const CanStatistics sum = SumStatistics(notifications);
    zassert_equal(sum.frames_received, FramesOf(notifications).size());
    zassert_equal(sum.frames_sent, 0);
    zassert_equal(sum.busy_us, 0, "A synthetic session has no bus time");
    zassert_equal(sum.filtered, 0);
    for(const uint64_t lost : sum.lost)
        zassert_equal(lost, 0);

    // The end record is the last one, after the final statistics.
    const auto ends = RecordsOf(notifications, CanRecordKind::END);
    zassert_equal(ends.size(), 1);
    zassert_equal(ends[0]->record.end_reason, CanEndReason::CLOSED);
    const auto& last_records = notifications.back().records;
    zassert_equal(&last_records.back(), ends[0]);
    if(last_records.size() >= 2)
        zassert_equal(last_records[last_records.size() - 2].record.kind, CanRecordKind::STATISTICS);
}

ZTEST(can_monitor_stream, test_a_stalled_link_overflows_into_gaps_and_the_end_follows) {
    Harness harness;
    harness.is_auto_complete = false;

    const auto answer = harness.OpenSynthetic(2000, 8);

    // 2000 frames a second of 13 bytes fill the 16 KiB ring in well under a second and a half.
    k_msleep(1500);
    zassert_equal(harness.notifier->Count(), CONFIG_EERIE_LEAP_CAN_MONITOR_IN_FLIGHT);

    // The link comes back while frames still arrive: the first one that finds room writes the gap.
    harness.is_auto_complete = true;
    harness.service->OnNotificationSent();
    k_msleep(300);

    harness.CloseAndDrain(answer.tok);

    const StreamSummary summary = Summarize(harness.notifier->Notifications());
    zassert_true(summary.gaps > 0, "The ring overflowed");
    zassert_true(summary.are_gaps_from_the_ring);
    zassert_true(summary.gap_frames > 0);
    zassert_true(summary.frames > 0);

    // Every frame is in the stream or counted lost; the gap records account for those they could be written for.
    const uint64_t lost = summary.statistics.lost[CanStatistics::LOST_RING];
    zassert_equal(summary.statistics.frames_received, summary.frames + lost);
    zassert_true(lost >= summary.gap_frames);
    zassert_true(lost - summary.gap_frames < 100, "Few losses are left without a gap");

    zassert_equal(summary.ends.size(), 1U);
    zassert_equal(summary.ends[0], CanEndReason::CLOSED);
    zassert_equal(summary.last_kind, CanRecordKind::END);
}

ZTEST(can_monitor_stream, test_a_pending_smp_response_holds_the_stream_back) {
    Harness harness;
    harness.is_smp_busy = true;

    const auto answer = harness.OpenSynthetic(200, 2);
    k_msleep(300);
    zassert_equal(harness.notifier->Count(), 0, "Nothing goes while an SMP response is pending");

    harness.is_smp_busy = false;
    zassert_true(WaitUntil([&] {
        return harness.notifier->Count() >= 1;
    }));

    harness.CloseAndDrain(answer.tok);

    // What was held back arrives whole.
    const auto notifications = DecodeAll(harness.notifier->Notifications());
    ExpectChain(notifications);
    zassert_true(FramesOf(notifications).size() >= 50);
}

ZTEST(can_monitor_stream, test_a_notify_without_a_buffer_is_retried_with_no_gap_in_seq) {
    Harness harness;
    harness.notifier->SetResult(-ENOMEM);

    const auto answer = harness.OpenSynthetic(500, 4);
    k_msleep(300);

    zassert_equal(harness.notifier->Count(), 0);
    zassert_true(harness.notifier->GetRefused() > 0, "The sender tried");

    harness.notifier->SetResult(0);
    zassert_true(WaitUntil([&] {
        return harness.notifier->Count() >= 1;
    }));
    k_msleep(200);
    harness.CloseAndDrain(answer.tok);

    // The first notification to get through is the first one built, with every frame since the start.
    const auto notifications = DecodeAll(harness.notifier->Notifications());
    ExpectChain(notifications);
    const auto frames = FramesOf(notifications);
    zassert_true(frames.size() > 100);
    for(size_t i = 0; i < frames.size(); ++i)
        zassert_equal(frames[i]->record.id, i % 4, "No frame went missing (%zu)", i);

    const CanStatistics sum = SumStatistics(notifications);
    zassert_equal(sum.lost[CanStatistics::LOST_RING], 0);
}

ZTEST(can_monitor_stream, test_a_refused_notification_is_dropped_and_seq_shows_the_hole) {
    Harness harness;
    harness.notifier->FailNext(-EIO, 1);

    const auto answer = harness.OpenSynthetic(200, 4);
    zassert_true(WaitUntil([&] {
        return harness.notifier->Count() >= 2;
    }));
    harness.CloseAndDrain(answer.tok);

    const auto notifications = DecodeAll(harness.notifier->Notifications());
    zassert_equal(harness.notifier->GetRefused(), 1);
    zassert_equal(notifications[0].sequence, 1, "Notification 0 was dropped");
    ExpectChain(notifications, 1);

    // The frames it held count as lost, so the books still balance.
    const CanStatistics sum = SumStatistics(notifications);
    const uint64_t lost = sum.lost[CanStatistics::LOST_RING];
    zassert_true(lost > 0);
    zassert_equal(sum.frames_received, FramesOf(notifications).size() + lost);

    // And the time the dropped records spanned is not lost: the next base_us picks up after them.
    const auto frames = FramesOf(notifications);
    zassert_true(notifications[0].base_us > 0);
    zassert_true(frames[0]->time_us >= notifications[0].base_us);
}

ZTEST(can_monitor_stream, test_stop_ends_the_sessions_for_a_new_configuration) {
    Harness harness;
    const auto answer = harness.OpenSynthetic(100, 2);
    k_msleep(100);

    zassert_true(harness.service->Stop());
    zassert_equal(harness.service->GetSessionCount(), 0);
    zassert_equal(harness.holds.load(), harness.releases.load(), "The fast link is released");

    zassert_true(WaitUntil([&] {
        return harness.HasEnded();
    }));
    const auto notifications = DecodeAll(harness.notifier->Notifications());
    const auto ends = RecordsOf(notifications, CanRecordKind::END);
    zassert_equal(ends.size(), 1);
    zassert_equal(ends[0]->record.end_reason, CanEndReason::CONFIGURATION_STORED);

    // The old token is gone, and nothing opens while the service is stopped.
    const auto closed = harness.service->Close(answer.tok);
    zassert_false(closed.has_value());
    zassert_equal(closed.error(), CanMonitorError::NO_SESSION);

    CanMonitorOpenRequest request;
    request.synthetic = CanSyntheticParameters{ .fps = 10, .ids = 1, .dlc = 1 };
    zassert_equal(harness.service->Open(request).error(), CanMonitorError::BUSY);

    zassert_true(harness.service->Start());
    zassert_true(harness.service->Open(request).has_value(), "The new configuration takes OPEN again");
}

ZTEST(can_monitor_stream, test_a_disconnect_ends_the_sessions_with_no_record) {
    Harness harness;
    harness.OpenSynthetic(100, 2);
    k_msleep(100);

    harness.service->OnDisconnected();

    zassert_equal(harness.service->GetSessionCount(), 0);
    zassert_equal(harness.holds.load(), harness.releases.load());

    // Nothing can drain into a link that is gone: the sender lets go of the session and of what is in flight.
    zassert_true(WaitUntil([&] {
        return harness.service->GetSenderSessionCount() == 0 && harness.service->GetInFlight() == 0;
    }));

    const size_t before = harness.notifier->Count();
    k_msleep(100);
    zassert_false(harness.HasEnded(), "No end record goes to a link that is gone");
    zassert_equal(harness.notifier->Count(), before, "Nothing more is sent");
}

ZTEST(can_monitor_stream, test_a_session_ending_with_no_central_subscribed_is_freed_at_once) {
    Harness harness;
    const auto answer = harness.OpenSynthetic(100, 2);
    k_msleep(100);

    harness.notifier->SetMaxSize(0);
    zassert_true(harness.service->Close(answer.tok).has_value());

    zassert_true(
        WaitUntil([&] {
            return harness.service->GetSenderSessionCount() == 0;
        }),
        "Nothing to drain into: the sender lets go of it"
    );

    // The port is free, and a new OPEN says why it can't stream.
    CanMonitorOpenRequest request;
    request.synthetic = CanSyntheticParameters{ .fps = 10, .ids = 1, .dlc = 1 };
    zassert_equal(harness.service->Open(request).error(), CanMonitorError::NOT_LISTENING);
}
