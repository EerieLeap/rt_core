#include <cstdlib>

#include <zephyr/ztest.h>

#include "can_monitor_test_support.h"

using namespace can_monitor_test;

namespace {} // namespace

ZTEST_SUITE(can_monitor_synthetic, NULL, NULL, NULL, ResetControllers, NULL);

ZTEST(can_monitor_synthetic, test_the_rate_holds_within_one_percent) {
    Harness harness;
    constexpr uint32_t FPS = 1000;
    constexpr uint32_t WINDOW_MS = 5000;

    const auto answer = harness.OpenSynthetic(FPS, 8);
    harness.Sleep(answer.tok, WINDOW_MS);
    harness.CloseAndDrain(answer.tok);

    const auto notifications = DecodeAll(harness.notifier->Notifications());
    ExpectChain(notifications);
    zassert_true(RecordsOf(notifications, CanRecordKind::GAP).empty(), "The ring never filled");

    // The frames' own times: the first at the start, the last a bit after its turn.
    const auto frames = RecordsOf(notifications, CanRecordKind::FRAME);
    zassert_true(frames.size() >= 4900, "Frames in 5 s: %zu", frames.size());

    const uint64_t span_us = frames.back()->time_us - frames.front()->time_us;
    const uint64_t expected_us = (frames.size() - 1) * 1000000ULL / FPS;
    const uint64_t error_us = span_us > expected_us ? span_us - expected_us : expected_us - span_us;
    zassert_true(
        error_us * 100 <= expected_us, "%zu frames took %u ms, %u ms are due", frames.size(),
        static_cast<unsigned>(span_us / 1000), static_cast<unsigned>(expected_us / 1000)
    );

    // And the records decode to the frames generated: a counter in the payload, the ids in turn.
    for(size_t i = 0; i < frames.size(); ++i) {
        zassert_equal(frames[i]->record.id, static_cast<uint32_t>(i % 8), "Frame %zu", i);
        zassert_equal(frames[i]->payload.size(), 8U);
        uint64_t counter = 0;
        for(size_t k = 0; k < 8; ++k)
            counter |= static_cast<uint64_t>(frames[i]->payload[k]) << (8 * k);
        zassert_equal(counter, i, "Counter of frame %zu", i);
    }

    const CanStatistics sum = SumStatistics(notifications);
    zassert_equal(sum.frames_received, frames.size());
}

ZTEST(can_monitor_synthetic, test_extended_fd_frames_decode_to_what_was_generated) {
    Harness harness;
    constexpr uint32_t IDS = 3;
    const CanSyntheticParameters parameters{ .fps = 200, .ids = IDS, .dlc = 15, .ext = true, .fd = true };

    const auto answer = harness.OpenSynthetic(parameters.fps, parameters.ids, parameters.dlc, true, true);
    k_msleep(500);
    harness.CloseAndDrain(answer.tok);

    const auto notifications = DecodeAll(harness.notifier->Notifications());
    ExpectChain(notifications);

    const auto frames = RecordsOf(notifications, CanRecordKind::FRAME);
    zassert_true(frames.size() > 20, "Frames: %zu", frames.size());
    for(size_t i = 0; i < frames.size(); ++i) {
        const CanRecord& record = frames[i]->record;
        zassert_equal(record.id, CanSyntheticSource::IdOf(parameters, i));
        zassert_true(record.id >= 0x18DA0000U && record.ide && record.fdf && record.brs);
        zassert_false(record.rtr || record.esi || record.tx);
        zassert_equal(record.dlc, 15);
        zassert_equal(frames[i]->payload.size(), 64U);
        for(size_t k = 0; k < 64; ++k)
            zassert_equal(frames[i]->payload[k], CanSyntheticSource::PayloadByte(i, k), "Frame %zu byte %zu", i, k);
    }
}

ZTEST(can_monitor_synthetic, test_parameters_that_cannot_be_generated_are_refused) {
    Harness harness;

    const auto open = [&](CanSyntheticParameters parameters) {
        CanMonitorOpenRequest request;
        request.synthetic = parameters;

        return harness.service->Open(request);
    };

    zassert_equal(open({ .fps = 0, .ids = 1, .dlc = 1 }).error(), CanMonitorError::UNKNOWN);
    zassert_equal(open({ .fps = 10, .ids = 0, .dlc = 1 }).error(), CanMonitorError::UNKNOWN);
    zassert_equal(open({ .fps = 10, .ids = 4096, .dlc = 1 }).error(), CanMonitorError::UNKNOWN);
    zassert_equal(open({ .fps = 10, .ids = 1, .dlc = 9 }).error(), CanMonitorError::UNKNOWN, "9 needs FD");
    zassert_equal(open({ .fps = 10, .ids = 1, .dlc = 16, .fd = true }).error(), CanMonitorError::UNKNOWN);
    zassert_equal(harness.service->GetSessionCount(), 0U);

    zassert_true(open({ .fps = 10, .ids = 1, .dlc = 9, .fd = true }).has_value());
}

ZTEST(can_monitor_synthetic, test_the_synthetic_port_takes_one_session) {
    Harness harness;
    const CanSyntheticParameters parameters{ .fps = 20, .ids = 2, .dlc = 2 };

    CanMonitorOpenRequest request;
    request.synthetic = parameters;
    const auto first = harness.service->Open(request);
    zassert_true(first.has_value());
    zassert_equal(first->ch, CAN_MONITOR_SYNTHETIC_PORT);
    zassert_equal(first->mode, CanMonitorMode::TAP);
    zassert_equal(first->type, eerie_leap::subsys::canbus::CanbusType::NONE);
    zassert_equal(first->bitrate, 0U);

    // The same request again is a lost answer; another one finds the port taken.
    const auto again = harness.service->Open(request);
    zassert_true(again.has_value());
    zassert_equal(again->tok, first->tok);
    zassert_equal(harness.service->GetSessionCount(), 1U);
    zassert_equal(harness.holds.load(), 1);

    request.synthetic->fps = 21;
    zassert_equal(harness.service->Open(request).error(), CanMonitorError::BUSY);
}
