#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>
#include <zephyr/ztest.h>

#include "can_monitor_test_support.h"
#include "domain/can_monitor_domain/smp/can_monitor_mgmt_group.h"
#include "smp_test_client.h"

using namespace can_monitor_test;
using namespace smp_test;

using eerie_leap::domain::can_monitor_domain::smp::CanMonitorMgmtGroup;

using Command = CanMonitorMgmtGroup::Command;

namespace {

constexpr SmpGroupId GROUP = SmpGroupId::CAN_MONITOR;

constexpr uint8_t Id(Command command) {
    return std::to_underlying(command);
}

void ExpectGroupError(const Response& response, CanMonitorError error) {
    zassert_true(response.error.has_value(), "Expected a group error");
    zassert_equal(response.error_group.value(), std::to_underlying(GROUP));
    zassert_equal(response.error.value(), std::to_underlying(error));
}

// The unit, its group and a client; the client goes first, then the group, then the unit.
struct GroupUnit {
    Harness harness;
    CanMonitorMgmtGroup group{ harness.service };
    SmpTestClient client;

    GroupUnit() {
        group.Register();
    }

    Response Write(Command command, const Bytes& body) {
        return client.Write(GROUP, Id(command), body);
    }
};

#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_TX

constexpr uint8_t FLAG_IDE = CAN_TX_FLAG_IDE;
constexpr uint8_t FLAG_FDF = CAN_TX_FLAG_FDF;
constexpr uint8_t FLAG_BRS = CAN_TX_FLAG_BRS;
constexpr uint8_t FLAG_RTR = CAN_TX_FLAG_RTR;

constexpr uint32_t REPLAY_CAPACITY = CONFIG_EERIE_LEAP_CAN_MONITOR_REPLAY_BUFFER_SIZE;

CanMonitorTxFrame MakeFrame(uint32_t id, uint8_t flags, std::initializer_list<uint8_t> data) {
    CanMonitorTxFrame frame;
    frame.id = id;
    frame.flags = flags;
    frame.size = static_cast<uint8_t>(data.size());
    std::ranges::copy(data, frame.data.begin());

    return frame;
}

CanMonitorPeriodic MakePeriodic(
    uint32_t slot,
    const CanMonitorTxFrame& frame,
    uint32_t period_ms,
    std::optional<uint32_t> count = std::nullopt
) {
    CanMonitorPeriodic entry;
    entry.slot = slot;
    entry.frame = frame;
    entry.period_ms = period_ms;
    entry.count = count;

    return entry;
}

// ACTIVE on can1, with the loopback controller handing frames back as the own-bus tests do.
CanMonitorOpenAnswer OpenActive(Harness& harness, uint32_t bitrate = 500000) {
    CanMonitorOpenRequest request;
    request.ch = UNCONFIGURED_PORT;
    request.mode = CanMonitorMode::ACTIVE;
    request.bitrate = bitrate;

    const auto answer = harness.service->Open(request);
    zassert_true(answer.has_value(), "OPEN failed (%d)", answer ? 0 : static_cast<int>(answer.error()));
    SwitchToLoopbackMode(UnconfiguredDevice());

    return *answer;
}

void Arm(Harness& harness, uint32_t token) {
    const auto state = harness.service->Arm(token, true);
    zassert_true(state.has_value() && state->is_armed, "ARM failed (%d)", state ? 0 : static_cast<int>(state.error()));
}

size_t CountSent(Harness& harness, uint32_t id) {
    return CountStreamFrames(harness.notifier->Notifications(), id, true);
}

size_t CountReceived(Harness& harness, uint32_t id) {
    return CountStreamFrames(harness.notifier->Notifications(), id, false);
}

std::vector<StreamTxEvent> Events(Harness& harness) {
    return StreamTxEvents(harness.notifier->Notifications());
}

size_t CountEvents(Harness& harness, CanTxEvent event) {
    const auto events = Events(harness);

    return static_cast<size_t>(std::ranges::count_if(events, [event](const StreamTxEvent& item) {
        return item.event == event;
    }));
}

// The count of the first event of this kind; fails the test if there is none.
uint64_t EventCount(Harness& harness, CanTxEvent event) {
    for(const StreamTxEvent& item : Events(harness)) {
        if(item.event == event)
            return item.count;
    }

    zassert_unreachable("No tx event %u", static_cast<unsigned>(event));
    return 0;
}

struct ReplayFrame {
    uint32_t id;
    uint64_t dt_us;
    std::vector<uint8_t> data;
};

// The records a chunk is made of.
Bytes ReplayRecords(const std::vector<ReplayFrame>& frames) {
    Bytes bytes;
    for(const ReplayFrame& frame : frames) {
        CanRecordFrame record;
        record.id = frame.id;
        record.dlc = static_cast<uint8_t>(frame.data.size());
        record.payload = frame.data;

        std::array<uint8_t, CAN_MAX_RECORD_SIZE> buffer{};
        const size_t size = EncodeFrame(buffer, frame.dt_us, record);
        zassert_true(size > 0);
        bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + size);
    }

    return bytes;
}

std::vector<ReplayFrame> SameFrames(uint32_t id, size_t count, uint64_t dt_us, size_t payload_size = 1) {
    return std::vector<ReplayFrame>(count, ReplayFrame{ id, dt_us, std::vector<uint8_t>(payload_size, 0x5A) });
}

CanMonitorReplayRequest Start(uint32_t speed_pct = 100, bool is_loop = false) {
    CanMonitorReplayRequest request;
    request.op = CanMonitorReplayOp::START;
    request.speed_pct = speed_pct;
    request.is_loop = is_loop;

    return request;
}

CanMonitorReplayRequest Chunk(uint16_t seq, const Bytes& data) {
    CanMonitorReplayRequest request;
    request.op = CanMonitorReplayOp::CHUNK;
    request.seq = seq;
    request.data = data;

    return request;
}

CanMonitorReplayRequest Op(CanMonitorReplayOp op) {
    CanMonitorReplayRequest request;
    request.op = op;

    return request;
}

// Plays a replay of @p frames to its end.
void PlayToTheEnd(Harness& harness, uint32_t token, const std::vector<ReplayFrame>& frames, uint32_t speed_pct) {
    const size_t done_before = CountEvents(harness, CanTxEvent::REPLAY_DONE);

    zassert_true(harness.service->Replay(token, Start(speed_pct)).has_value());
    zassert_true(harness.service->Replay(token, Chunk(0, ReplayRecords(frames))).has_value());
    zassert_true(harness.service->Replay(token, Op(CanMonitorReplayOp::FINISH)).has_value());

    zassert_true(
        WaitUntil(
            [&] {
                return CountEvents(harness, CanTxEvent::REPLAY_DONE) > done_before;
            },
            8000
        ),
        "The replay ends"
    );
}

#endif // CONFIG_EERIE_LEAP_CAN_MONITOR_TX

} // namespace

ZTEST_SUITE(can_monitor_transmit, NULL, NULL, NULL, ResetControllers, NULL);

#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_TX

ZTEST(can_monitor_transmit, test_nothing_is_sent_until_the_session_is_armed) {
    Harness harness;
    const auto answer = OpenActive(harness);
    const std::array<CanMonitorTxFrame, 1> frames = { MakeFrame(0x123, 0, { 1, 2, 3 }) };

    zassert_equal(harness.service->Send(answer.tok, frames).error(), CanMonitorError::NOT_ARMED);
    zassert_equal(harness.service->SetPeriodic(answer.tok, {}).error(), CanMonitorError::NOT_ARMED);
    zassert_equal(harness.service->Replay(answer.tok, Start()).error(), CanMonitorError::NOT_ARMED);

    const auto armed = harness.service->Arm(answer.tok, true);
    zassert_true(armed.has_value());
    zassert_true(armed->is_armed);
    zassert_equal(armed->share_pct, CONFIG_EERIE_LEAP_CAN_MONITOR_TX_SHARE_PERCENT);
    zassert_true(armed->reserved.empty(), "can1 is not the COM channel");

    const auto sent = harness.service->Send(answer.tok, frames);
    zassert_true(sent.has_value());
    zassert_equal(sent->sent, 1U);
    zassert_equal(sent->refused, 0U);

    const auto disarmed = harness.service->Arm(answer.tok, false);
    zassert_true(disarmed.has_value());
    zassert_false(disarmed->is_armed);
    zassert_equal(harness.service->Send(answer.tok, frames).error(), CanMonitorError::NOT_ARMED);
}

ZTEST(can_monitor_transmit, test_only_tap_and_active_sessions_arm) {
    Harness harness;

    const auto synthetic = harness.OpenSynthetic(20, 1, 1);
    zassert_equal(harness.service->Arm(synthetic.tok, true).error(), CanMonitorError::BAD_MODE);

    CanMonitorOpenRequest request;
    request.ch = UNCONFIGURED_PORT;
    request.mode = CanMonitorMode::LISTEN_ONLY;
    request.bitrate = 500000;
    const auto listen_only = harness.service->Open(request);
    zassert_true(listen_only.has_value());
    zassert_equal(harness.service->Arm(listen_only->tok, true).error(), CanMonitorError::BAD_MODE);

    const std::array<CanMonitorTxFrame, 1> frames = { MakeFrame(0x123, 0, { 1 }) };
    zassert_equal(harness.service->Send(listen_only->tok, frames).error(), CanMonitorError::BAD_MODE);

    zassert_equal(harness.service->Arm(0, true).error(), CanMonitorError::BAD_TOKEN);
    zassert_equal(harness.service->Arm(synthetic.tok + 1, true).error(), CanMonitorError::NO_SESSION);

    // A TAP session on a configured channel arms.
    zassert_true(harness.service->Close(listen_only->tok).has_value());
    CanMonitorOpenRequest tap;
    tap.ch = CONFIGURED_PORT;
    const auto tapped = harness.service->Open(tap);
    zassert_true(tapped.has_value());
    zassert_true(harness.service->Arm(tapped->tok, true).has_value());
}

ZTEST(can_monitor_transmit, test_frames_sent_come_back_as_received_and_as_sent) {
    Harness harness;
    const auto answer = OpenActive(harness);
    Arm(harness, answer.tok);

    const std::array<CanMonitorTxFrame, 3> frames = {
        MakeFrame(0x123, 0, { 1, 2, 3 }),
        MakeFrame(0x18DAF110, FLAG_IDE, { 1, 2, 3, 4, 5, 6, 7, 8 }),
        // A remote frame asks for 4 bytes: the bytes given only say how many.
        MakeFrame(0x124, FLAG_RTR, { 9, 9, 9, 9 }),
    };
    const auto result = harness.service->Send(answer.tok, frames);
    zassert_true(result.has_value());
    zassert_equal(result->sent, 3U);
    zassert_equal(result->refused, 0U);

    for(const uint32_t id : { 0x123U, 0x18DAF110U, 0x124U }) {
        zassert_true(
            WaitUntil([&] {
                return CountReceived(harness, id) == 1 && CountSent(harness, id) == 1;
            }),
            "Frame 0x%x is received and sent", id
        );
    }

    harness.CloseAndDrain(answer.tok);

    const auto notifications = DecodeAll(harness.notifier->Notifications());
    for(const DecodedRecord* record : RecordsOf(notifications, CanRecordKind::FRAME)) {
        const CanRecord& frame = record->record;
        if(frame.id == 0x123U) {
            zassert_false(frame.ide || frame.rtr);
            zassert_equal(record->payload, (Bytes{ 1, 2, 3 }));
        } else if(frame.id == 0x18DAF110U) {
            zassert_true(frame.ide);
            zassert_equal(record->payload, (Bytes{ 1, 2, 3, 4, 5, 6, 7, 8 }));
        } else {
            zassert_true(frame.rtr);
            zassert_equal(frame.dlc, 4);
            zassert_true(record->payload.empty());
        }
        zassert_false(frame.tx_failed);
    }
}

ZTEST(can_monitor_transmit, test_a_frame_no_bus_carries_refuses_the_whole_request) {
    Harness harness;
    const auto answer = OpenActive(harness);
    Arm(harness, answer.tok);

    const CanMonitorTxFrame good = MakeFrame(0x100, 0, { 1 });
    const std::vector<CanMonitorTxFrame> bad_frames = {
        MakeFrame(0x101, FLAG_BRS, { 1 }),                         // BRS without FDF
        MakeFrame(0x101, FLAG_FDF, { 1 }),                         // FD on a classical channel
        MakeFrame(0x101, 0, { 1, 2, 3, 4, 5, 6, 7, 8, 9 }),        // more than 8 bytes without FDF
        MakeFrame(0x101, FLAG_FDF | FLAG_RTR, {}),                 // a remote frame has no FD
        MakeFrame(0x101, FLAG_RTR, { 1, 2, 3, 4, 5, 6, 7, 8, 9 }), // asks for more than 8 bytes
        MakeFrame(0x101, 0x10, { 1 }),                             // a flag nobody has
        MakeFrame(0x800, 0, { 1 }),                                // does not fit 11 bits
        MakeFrame(0x20000000, FLAG_IDE, { 1 }),                    // does not fit 29 bits
    };

    for(const CanMonitorTxFrame& bad : bad_frames) {
        const std::array<CanMonitorTxFrame, 2> request = { good, bad };
        zassert_equal(
            harness.service->Send(answer.tok, request).error(), CanMonitorError::INVALID, "id 0x%x flags %u", bad.id,
            bad.flags
        );
    }

    // No frames at all, and more than a request takes.
    zassert_equal(harness.service->Send(answer.tok, {}).error(), CanMonitorError::INVALID);
    const std::vector<CanMonitorTxFrame> too_many(17, good);
    zassert_equal(harness.service->Send(answer.tok, too_many).error(), CanMonitorError::INVALID);

    // None of the good frames went.
    k_msleep(150);
    zassert_equal(CountSent(harness, 0x100), 0U);
}

ZTEST(can_monitor_transmit, test_the_cap_lets_a_burst_through_then_refuses_then_frees_again) {
    Harness harness;
    // A slow bus, so that the frames of a request cost more bus time than the cap gives back while it is sent.
    const auto answer = OpenActive(harness, 20000);
    Arm(harness, answer.tok);

    const std::vector<CanMonitorTxFrame> frames(16, MakeFrame(0x100, 0, { 1, 2, 3, 4, 5, 6, 7, 8 }));
    const auto burst = harness.service->Send(answer.tok, frames);
    zassert_true(burst.has_value());
    zassert_true(burst->sent >= CONFIG_EERIE_LEAP_CAN_MONITOR_TX_BURST_FRAMES, "A burst of %u", burst->sent);
    zassert_true(burst->refused > 0, "Not all of 16");
    zassert_equal(burst->sent + burst->refused, 16U);

    // The bucket is empty: not one more.
    const std::array<CanMonitorTxFrame, 1> one = { MakeFrame(0x100, 0, { 1, 2, 3, 4, 5, 6, 7, 8 }) };
    zassert_equal(harness.service->Send(answer.tok, one).error(), CanMonitorError::RATE);

    // And it fills again with time.
    k_msleep(300);
    const auto again = harness.service->Send(answer.tok, one);
    zassert_true(again.has_value());
    zassert_equal(again->sent, 1U);

    zassert_true(WaitUntil([&] {
        return CountSent(harness, 0x100) == burst->sent + 1;
    }));
}

ZTEST(can_monitor_transmit, test_the_com_channels_identifiers_are_reserved) {
    Harness harness(true);

    CanMonitorOpenRequest request;
    request.ch = CONFIGURED_PORT;
    const auto answer = harness.service->Open(request);
    zassert_true(answer.has_value());

    const auto com = harness.canbus_service->GetComConfiguration();
    const uint32_t cdmp_last = com.cdmp_base_can_id + 99;
    const uint32_t smp_last = com.smp_can_id_base | 0xFFFF;
    zassert_true(cdmp_last + 1 <= CAN_STD_ID_MASK);

    const auto armed = harness.service->Arm(answer->tok, true);
    zassert_true(armed.has_value());
    zassert_equal(armed->reserved.size(), 2U);
    zassert_equal(armed->reserved[0].from, com.cdmp_base_can_id);
    zassert_equal(armed->reserved[0].to, cdmp_last);
    zassert_false(armed->reserved[0].ext);
    zassert_equal(armed->reserved[1].from, com.smp_can_id_base);
    zassert_equal(armed->reserved[1].to, smp_last);
    zassert_true(armed->reserved[1].ext);

    // A request with one reserved frame sends none.
    const std::array<CanMonitorTxFrame, 2> mixed = { MakeFrame(cdmp_last + 1, 0, { 1 }),
        MakeFrame(com.cdmp_base_can_id + 5, 0, { 1 }) };
    zassert_equal(harness.service->Send(answer->tok, mixed).error(), CanMonitorError::RESERVED);
    const std::array<CanMonitorTxFrame, 1> network_ext = { MakeFrame(com.smp_can_id_base + 7, FLAG_IDE, { 1 }) };
    zassert_equal(harness.service->Send(answer->tok, network_ext).error(), CanMonitorError::RESERVED);

    const std::array<CanMonitorPeriodic, 1> periodic = {
        MakePeriodic(1, MakeFrame(com.cdmp_base_can_id, 0, { 1 }), 100)
    };
    zassert_equal(harness.service->SetPeriodic(answer->tok, periodic).error(), CanMonitorError::RESERVED);

    // The first identifier past the range is free.
    const std::array<CanMonitorTxFrame, 1> free_id = { MakeFrame(cdmp_last + 1, 0, { 1 }) };
    zassert_true(harness.service->Send(answer->tok, free_id).has_value());
    k_msleep(100);
    zassert_equal(CountSent(harness, cdmp_last + 1), 1U, "Only the free frame went");

    // A replay skips what is reserved, counts it, and plays the rest.
    zassert_true(harness.service->Replay(answer->tok, Start()).has_value());
    const std::vector<ReplayFrame> frames = { { com.cdmp_base_can_id + 1, 10'000, { 1 } }, { 0x600, 10'000, { 2 } } };
    zassert_true(harness.service->Replay(answer->tok, Chunk(0, ReplayRecords(frames))).has_value());
    zassert_true(harness.service->Replay(answer->tok, Op(CanMonitorReplayOp::FINISH)).has_value());
    zassert_true(WaitUntil([&] {
        return CountEvents(harness, CanTxEvent::REPLAY_DONE) == 1;
    }));
    zassert_equal(CountSent(harness, com.cdmp_base_can_id + 1), 0U);
    zassert_equal(EventCount(harness, CanTxEvent::FRAMES_SKIPPED), 1U);
}

ZTEST(can_monitor_transmit, test_periodic_frames_repeat_until_their_count_or_the_list_is_replaced) {
    Harness harness;
    const auto answer = OpenActive(harness);
    Arm(harness, answer.tok);

    const std::array<CanMonitorPeriodic, 2> list = {
        MakePeriodic(1, MakeFrame(0x321, 0, { 1, 2 }), 20),
        MakePeriodic(2, MakeFrame(0x322, 0, { 3 }), 50, 3),
    };
    zassert_true(harness.service->SetPeriodic(answer.tok, list).has_value());

    // The one with a count stops at it; the other goes on.
    zassert_true(WaitUntil([&] {
        return CountSent(harness, 0x322) == 3;
    }));
    zassert_true(WaitUntil([&] {
        return CountSent(harness, 0x321) >= 5;
    }));
    k_msleep(300);
    zassert_equal(CountSent(harness, 0x322), 3U);

    // A request that cannot be kept leaves the list as it was.
    const std::array<CanMonitorPeriodic, 2> same_slot = {
        MakePeriodic(1, MakeFrame(0x400, 0, { 1 }), 20),
        MakePeriodic(1, MakeFrame(0x401, 0, { 1 }), 20),
    };
    zassert_equal(harness.service->SetPeriodic(answer.tok, same_slot).error(), CanMonitorError::INVALID);
    const std::array<CanMonitorPeriodic, 1> no_period = { MakePeriodic(1, MakeFrame(0x400, 0, { 1 }), 0) };
    zassert_equal(harness.service->SetPeriodic(answer.tok, no_period).error(), CanMonitorError::INVALID);
    const std::array<CanMonitorPeriodic, 1> fd_frame = { MakePeriodic(1, MakeFrame(0x400, FLAG_FDF, { 1 }), 20) };
    zassert_equal(harness.service->SetPeriodic(answer.tok, fd_frame).error(), CanMonitorError::INVALID);
    std::vector<CanMonitorPeriodic> too_many;
    for(uint32_t slot = 0; slot <= CONFIG_EERIE_LEAP_CAN_MONITOR_PERIODIC_SLOTS; ++slot)
        too_many.push_back(MakePeriodic(slot, MakeFrame(0x400, 0, { 1 }), 1000));
    zassert_equal(harness.service->SetPeriodic(answer.tok, too_many).error(), CanMonitorError::INVALID);
    zassert_equal(CountSent(harness, 0x400), 0U);

    const size_t before = CountSent(harness, 0x321);
    k_msleep(150);
    zassert_true(CountSent(harness, 0x321) > before, "The old list still runs");

    // An empty list stops them all.
    zassert_true(harness.service->SetPeriodic(answer.tok, {}).has_value());
    k_msleep(100);
    const size_t stopped = CountSent(harness, 0x321);
    k_msleep(200);
    zassert_equal(CountSent(harness, 0x321), stopped);
}

ZTEST(can_monitor_transmit, test_disarming_reports_the_frames_that_were_not_sent) {
    Harness harness;
    const auto answer = OpenActive(harness);
    Arm(harness, answer.tok);

    // The first of 5 goes at once; the replay's frames are due in 5 s.
    const std::array<CanMonitorPeriodic, 1> list = { MakePeriodic(1, MakeFrame(0x321, 0, { 1 }), 5000, 5) };
    zassert_true(harness.service->SetPeriodic(answer.tok, list).has_value());
    zassert_true(WaitUntil([&] {
        return CountSent(harness, 0x321) == 1;
    }));

    zassert_true(harness.service->Replay(answer.tok, Start()).has_value());
    zassert_true(
        harness.service->Replay(answer.tok, Chunk(0, ReplayRecords(SameFrames(0x500, 10, 5'000'000)))).has_value()
    );

    const auto disarmed = harness.service->Arm(answer.tok, false);
    zassert_true(disarmed.has_value());

    zassert_true(WaitUntil([&] {
        return CountEvents(harness, CanTxEvent::DISARMED) == 1;
    }));
    zassert_equal(EventCount(harness, CanTxEvent::DISARMED), 4U + 10U, "4 periodic and 10 replay frames left");

    // Nothing runs any more.
    k_msleep(100);
    zassert_equal(CountSent(harness, 0x321), 1U);
    zassert_equal(CountSent(harness, 0x500), 0U);

    // Closing an armed session reports it too, before the end.
    Arm(harness, answer.tok);
    zassert_true(harness.service->SetPeriodic(answer.tok, list).has_value());
    harness.CloseAndDrain(answer.tok);

    std::vector<CanRecordKind> kinds;
    WalkStream(harness.notifier->Notifications(), [&](const CanRecord& record, uint64_t) {
        if(record.kind == CanRecordKind::TX_EVENT || record.kind == CanRecordKind::END)
            kinds.push_back(record.kind);
    });
    zassert_equal(kinds.size(), 3U);
    zassert_equal(kinds[1], CanRecordKind::TX_EVENT);
    zassert_equal(kinds[2], CanRecordKind::END);
    zassert_equal(CountEvents(harness, CanTxEvent::DISARMED), 2U);
}

ZTEST(can_monitor_transmit, test_a_lapsed_lease_stops_the_periodic_frames_and_the_replay) {
    Harness harness;
    const auto answer = OpenActive(harness);
    Arm(harness, answer.tok);

    const std::array<CanMonitorPeriodic, 1> list = { MakePeriodic(1, MakeFrame(0x321, 0, { 1 }), 20) };
    zassert_true(harness.service->SetPeriodic(answer.tok, list).has_value());
    zassert_true(harness.service->Replay(answer.tok, Start()).has_value());
    zassert_true(
        harness.service->Replay(answer.tok, Chunk(0, ReplayRecords(SameFrames(0x500, 30, 1'000'000)))).has_value()
    );

    // Nobody asks: the lease lapses with the frames still going.
    zassert_true(WaitUntil(
        [&] {
            return harness.service->GetSessionCount() == 0;
        },
        3 * CONFIG_EERIE_LEAP_CAN_MONITOR_LEASE_MS
    ));
    zassert_true(WaitUntil([&] {
        return harness.HasEnded();
    }));

    const StreamSummary summary = Summarize(harness.notifier->Notifications());
    zassert_equal(summary.ends[0], CanEndReason::LEASE);
    zassert_equal(CountEvents(harness, CanTxEvent::DISARMED), 1U);
    zassert_true(EventCount(harness, CanTxEvent::DISARMED) >= 20U, "Most of the replay's 30 frames were left");

    const size_t periodic = CountSent(harness, 0x321);
    const size_t replayed = CountSent(harness, 0x500);
    zassert_true(periodic > 10U);
    k_msleep(250);
    zassert_equal(CountSent(harness, 0x321), periodic);
    zassert_equal(CountSent(harness, 0x500), replayed);
}

ZTEST(can_monitor_transmit, test_bus_off_stops_everything_until_the_bus_is_back) {
    Harness harness;
    const auto answer = OpenActive(harness);
    Arm(harness, answer.tok);

    const std::array<CanMonitorPeriodic, 1> list = { MakePeriodic(1, MakeFrame(0x321, 0, { 1 }), 20) };
    zassert_true(harness.service->SetPeriodic(answer.tok, list).has_value());
    zassert_true(harness.service->Replay(answer.tok, Start()).has_value());
    zassert_true(
        harness.service->Replay(answer.tok, Chunk(0, ReplayRecords(SameFrames(0x500, 20, 1'000'000)))).has_value()
    );
    zassert_true(WaitUntil([&] {
        return CountSent(harness, 0x321) >= 2;
    }));

    Canbus* bus = harness.service->GetOwnBus(answer.tok);
    zassert_not_null(bus);
    bus->OnBusStateChanged(CAN_STATE_BUS_OFF, { .tx_err_cnt = 255, .rx_err_cnt = 0 });

    zassert_true(WaitUntil([&] {
        return CountEvents(harness, CanTxEvent::BUS_OFF) == 1;
    }));
    zassert_true(EventCount(harness, CanTxEvent::BUS_OFF) >= 15U, "The replay's frames were not sent");

    k_msleep(100);
    const size_t sent = CountSent(harness, 0x321);
    k_msleep(150);
    zassert_equal(CountSent(harness, 0x321), sent);
    zassert_equal(CountSent(harness, 0x500), 0U);

    // Disarmed, and not armed again while the bus is off.
    const std::array<CanMonitorTxFrame, 1> one = { MakeFrame(0x100, 0, { 1 }) };
    zassert_equal(harness.service->Send(answer.tok, one).error(), CanMonitorError::NOT_ARMED);
    zassert_equal(harness.service->Arm(answer.tok, true).error(), CanMonitorError::BUS_OFF);

    bus->OnBusStateChanged(CAN_STATE_ERROR_ACTIVE, {});
    zassert_true(WaitUntil([&] {
        return harness.service->Arm(answer.tok, true).has_value();
    }));
}

ZTEST(can_monitor_transmit, test_a_replay_plays_at_the_pace_of_the_capture_and_at_twice_that) {
    Harness harness;
    const auto answer = OpenActive(harness);
    Arm(harness, answer.tok);

    // Five frames 100 ms apart: four gaps, 400 ms at the capture's pace.
    PlayToTheEnd(harness, answer.tok, SameFrames(0x500, 5, 100'000), 100);
    PlayToTheEnd(harness, answer.tok, SameFrames(0x501, 5, 100'000), 200);

    zassert_equal(EventCount(harness, CanTxEvent::REPLAY_DONE), 5U, "Frames played");

    const auto normal = StreamSendTimes(harness.notifier->Notifications(), 0x500);
    const auto fast = StreamSendTimes(harness.notifier->Notifications(), 0x501);
    zassert_equal(normal.size(), 5U);
    zassert_equal(fast.size(), 5U);

    const uint64_t normal_us = normal.back() - normal.front();
    const uint64_t fast_us = fast.back() - fast.front();
    zassert_true(
        normal_us >= 250'000 && normal_us <= 800'000, "100 percent took %u ms", static_cast<unsigned>(normal_us / 1000)
    );
    zassert_true(
        fast_us >= 100'000 && fast_us <= 400'000, "200 percent took %u ms", static_cast<unsigned>(fast_us / 1000)
    );
    zassert_true(fast_us * 4 < normal_us * 3, "Twice as fast is faster");
}

ZTEST(can_monitor_transmit, test_a_replay_over_the_cap_waits_instead_of_losing_frames) {
    Harness harness;
    const auto answer = OpenActive(harness, 20000);
    Arm(harness, answer.tok);

    // All due at once, more than the bucket holds: the rest wait for the bus time.
    PlayToTheEnd(harness, answer.tok, SameFrames(0x500, 20, 0, 8), 100);

    zassert_equal(EventCount(harness, CanTxEvent::REPLAY_DONE), 20U);
    zassert_equal(CountEvents(harness, CanTxEvent::FRAMES_SKIPPED), 0U);

    const auto times = StreamSendTimes(harness.notifier->Notifications(), 0x500);
    zassert_equal(times.size(), 20U);
    zassert_true(
        times.back() - times.front() >= 50'000, "The cap spread them over %u ms",
        static_cast<unsigned>((times.back() - times.front()) / 1000)
    );
}

ZTEST(can_monitor_transmit, test_a_replay_refuses_what_it_cannot_take_and_reports_underruns) {
    Harness harness;
    const auto answer = OpenActive(harness);
    Arm(harness, answer.tok);

    // No replay yet.
    zassert_equal(
        harness.service->Replay(answer.tok, Chunk(0, ReplayRecords(SameFrames(0x600, 1, 0)))).error(),
        CanMonitorError::BAD_SEQ
    );
    zassert_equal(
        harness.service->Replay(answer.tok, Op(CanMonitorReplayOp::FINISH)).error(), CanMonitorError::BAD_SEQ
    );

    const auto started = harness.service->Replay(answer.tok, Start());
    zassert_true(started.has_value());
    zassert_equal(*started, REPLAY_CAPACITY);
    zassert_equal(harness.service->Replay(answer.tok, Start(0)).error(), CanMonitorError::INVALID);
    zassert_equal(harness.service->Replay(answer.tok, Start(100'000)).error(), CanMonitorError::INVALID);

    // Out of order, malformed, too large: none is taken, so the first chunk may still come.
    zassert_equal(
        harness.service->Replay(answer.tok, Chunk(1, ReplayRecords(SameFrames(0x600, 1, 0)))).error(),
        CanMonitorError::BAD_SEQ
    );
    zassert_equal(
        harness.service->Replay(answer.tok, Chunk(0, Bytes{ 0xFF, 0xFF, 0xFF })).error(), CanMonitorError::INVALID
    );
    zassert_equal(
        harness.service->Replay(answer.tok, Chunk(0, ReplayRecords(SameFrames(0x600, 2 * REPLAY_CAPACITY / 6, 0, 2))))
            .error(),
        CanMonitorError::NO_ROOM
    );

    // Two frames, then nothing: an underrun, and the replay waits for the app.
    const auto first = harness.service->Replay(answer.tok, Chunk(0, ReplayRecords(SameFrames(0x600, 2, 10'000))));
    zassert_true(first.has_value());
    zassert_true(*first < REPLAY_CAPACITY);
    zassert_true(WaitUntil([&] {
        return CountSent(harness, 0x600) == 2 && CountEvents(harness, CanTxEvent::REPLAY_UNDERRUN) == 1;
    }));
    zassert_equal(EventCount(harness, CanTxEvent::REPLAY_UNDERRUN), 1U);

    // It goes on when data comes.
    zassert_true(
        harness.service->Replay(answer.tok, Chunk(1, ReplayRecords(SameFrames(0x600, 1, 10'000)))).has_value()
    );
    zassert_true(WaitUntil([&] {
        return CountSent(harness, 0x600) == 3 && CountEvents(harness, CanTxEvent::REPLAY_UNDERRUN) == 2;
    }));

    zassert_true(harness.service->Replay(answer.tok, Op(CanMonitorReplayOp::FINISH)).has_value());
    zassert_true(WaitUntil([&] {
        return CountEvents(harness, CanTxEvent::REPLAY_DONE) == 1;
    }));
    zassert_equal(EventCount(harness, CanTxEvent::REPLAY_DONE), 3U);

    // Over: nothing to add to, and stopping what is not running frees what there is.
    zassert_equal(
        harness.service->Replay(answer.tok, Chunk(2, ReplayRecords(SameFrames(0x600, 1, 0)))).error(),
        CanMonitorError::BAD_SEQ
    );
    const auto stopped = harness.service->Replay(answer.tok, Op(CanMonitorReplayOp::STOP));
    zassert_true(stopped.has_value());
    zassert_equal(*stopped, REPLAY_CAPACITY);

    // A replay stopped halfway sends no more.
    zassert_true(harness.service->Replay(answer.tok, Start()).has_value());
    zassert_true(
        harness.service->Replay(answer.tok, Chunk(0, ReplayRecords(SameFrames(0x601, 3, 200'000)))).has_value()
    );
    zassert_true(harness.service->Replay(answer.tok, Op(CanMonitorReplayOp::STOP)).has_value());
    k_msleep(500);
    zassert_equal(CountSent(harness, 0x601), 0U);
    zassert_equal(
        harness.service->Replay(answer.tok, Chunk(1, ReplayRecords(SameFrames(0x601, 1, 0)))).error(),
        CanMonitorError::BAD_SEQ
    );
}

// Waits for @p predicate like WaitUntil(), asking for the session's stats each time so that its lease is renewed.
template<typename Predicate>
bool WaitRenewing(Harness& harness, uint32_t token, Predicate predicate, int timeout_ms = 3000) {
    return WaitUntil(
        [&] {
            [[maybe_unused]] const auto stats = harness.service->GetStats(token);

            return predicate();
        },
        timeout_ms
    );
}

ZTEST(can_monitor_transmit, test_skipped_frames_are_reported_within_a_second_when_no_more_are_skipped) {
    Harness harness;
    // A slow bus, and a frame that takes 13.1 ms of it: more than the cap gives back (half of the time) in one
    // tick of a kernel with 10 ms ticks, which a work item's delay rounds up to.
    const auto answer = OpenActive(harness, 10000);
    Arm(harness, answer.tok);

    // Empty the bucket first, so that the periodic frame is over the cap from its first run.
    const std::vector<CanMonitorTxFrame> burst(16, MakeFrame(0x18DAF110, FLAG_IDE, { 1, 2, 3, 4, 5, 6, 7, 8 }));
    for(int attempts = 0; attempts < 8; ++attempts) {
        const auto result = harness.service->Send(answer.tok, burst);
        if(!result.has_value()) {
            zassert_equal(result.error(), CanMonitorError::RATE);
            break;
        }
    }

    const std::array<CanMonitorPeriodic, 1> list = {
        MakePeriodic(1, MakeFrame(0x18DAF111, FLAG_IDE, { 1, 2, 3, 4, 5, 6, 7, 8 }), 1)
    };
    zassert_true(harness.service->SetPeriodic(answer.tok, list).has_value());

    // The first skipped frame is reported at once.
    zassert_true(
        WaitRenewing(
            harness, answer.tok,
            [&] {
                return CountEvents(harness, CanTxEvent::FRAMES_SKIPPED) >= 1;
            }
        ),
        "The first skipped frame is reported"
    );

    // More are skipped within the second, then none: the list is empty and the session still armed.
    harness.Sleep(answer.tok, 300);
    zassert_true(harness.service->SetPeriodic(answer.tok, {}).has_value());
    const size_t reported = CountEvents(harness, CanTxEvent::FRAMES_SKIPPED);

    // What was skipped since the report goes out when the second is over, with nothing to prompt it.
    zassert_true(
        WaitRenewing(
            harness, answer.tok,
            [&] {
                return CountEvents(harness, CanTxEvent::FRAMES_SKIPPED) > reported;
            },
            3000
        ),
        "The rest is reported within the second"
    );

    zassert_equal(CountEvents(harness, CanTxEvent::DISARMED), 0U, "Still armed");
    zassert_true(Events(harness).back().count > 0);
}

ZTEST(can_monitor_transmit, test_more_events_than_a_stalled_link_can_carry_do_not_push_out_the_last) {
    Harness harness;
    const auto answer = OpenActive(harness);
    Arm(harness, answer.tok);

    // An SMP answer holds the sender back: whatever it has to say waits.
    harness.is_smp_busy = true;

    // 20 underruns: the buffer is empty after each frame, and the replay is not finished.
    zassert_true(harness.service->Replay(answer.tok, Start()).has_value());
    for(uint16_t seq = 0; seq < 20; ++seq) {
        zassert_true(
            harness.service->Replay(answer.tok, Chunk(seq, ReplayRecords(SameFrames(0x500, 1, 0)))).has_value()
        );
        k_msleep(40);
    }
    zassert_true(harness.service->Arm(answer.tok, false).has_value());
    k_msleep(50);

    harness.is_smp_busy = false;
    zassert_true(WaitUntil([&] {
        return CountEvents(harness, CanTxEvent::DISARMED) == 1;
    }));

    // The underruns arrive as one record with the number so far, and "disarmed" is the last event.
    const auto events = Events(harness);
    zassert_equal(events.back().event, CanTxEvent::DISARMED);
    zassert_equal(CountEvents(harness, CanTxEvent::REPLAY_UNDERRUN), 1U);
    zassert_true(
        EventCount(harness, CanTxEvent::REPLAY_UNDERRUN) >= 17U, "Underruns so far: %u",
        static_cast<unsigned>(EventCount(harness, CanTxEvent::REPLAY_UNDERRUN))
    );
    zassert_true(CountSent(harness, 0x500) >= 17U);
}

// OPEN through the group for ACTIVE on can1, with the loopback controller handing frames back. The tests below
// are small and share this, so that each keeps little on the stack: the SMP client's request and response
// objects are large on the QEMU targets.
static uint32_t OpenActiveThroughGroup(GroupUnit& unit) {
    const auto opened = unit.Write(
        Command::OPEN, OpenBody().Put("ch", UNCONFIGURED_PORT).Put("mode", 2).Put("bitrate", 500000).Build()
    );
    zassert_true(opened.IsOk());
    SwitchToLoopbackMode(UnconfiguredDevice());

    return opened.Value("tok");
}

static void ArmThroughGroup(GroupUnit& unit, uint32_t token) {
    const auto armed = unit.Write(Command::ARM, CborMap().Put("tok", token).PutBool("on", true).Build());
    zassert_true(armed.IsOk());
    zassert_true(armed.Flag("armed"));
    zassert_equal(armed.Value("share_pct"), static_cast<uint32_t>(CONFIG_EERIE_LEAP_CAN_MONITOR_TX_SHARE_PERCENT));
    zassert_equal(armed.list_sizes.at("reserved"), 0U);
}

ZTEST(can_monitor_transmit, test_the_group_arms_and_sends) {
    // On the heap: the group keeps buffers for the lists it decodes, which is more than a test's stack likes.
    const auto unit_owner = std::make_unique<GroupUnit>();
    GroupUnit& unit = *unit_owner;
    const uint32_t token = OpenActiveThroughGroup(unit);

    const std::array<OpenBody::FrameSpec, 2> two_frames = { OpenBody::FrameSpec{ 0x123, 0, { 1, 2, 3 } },
        OpenBody::FrameSpec{ 0x18DAF110, FLAG_IDE, { 4 } } };
    ExpectGroupError(
        unit.Write(Command::SEND, OpenBody().Put("tok", token).PutFrames(two_frames).Build()),
        CanMonitorError::NOT_ARMED
    );

    ArmThroughGroup(unit, token);

    {
        const auto sent = unit.Write(Command::SEND, OpenBody().Put("tok", token).PutFrames(two_frames).Build());
        zassert_true(sent.IsOk());
        zassert_equal(sent.Value("sent"), 2U);
        zassert_equal(sent.Value("refused"), 0U);
    }
    zassert_true(WaitUntil([&] {
        return CountSent(unit.harness, 0x123) == 1 && CountSent(unit.harness, 0x18DAF110) == 1;
    }));

    // An unknown session.
    ExpectGroupError(
        unit.Write(Command::SEND, OpenBody().Put("tok", token + 1).PutFrames(two_frames).Build()),
        CanMonitorError::NO_SESSION
    );
}

ZTEST(can_monitor_transmit, test_the_group_refuses_requests_that_are_malformed) {
    // On the heap: the group keeps buffers for the lists it decodes, which is more than a test's stack likes.
    const auto unit_owner = std::make_unique<GroupUnit>();
    GroupUnit& unit = *unit_owner;
    const uint32_t token = OpenActiveThroughGroup(unit);
    ArmThroughGroup(unit, token);

    // More frames than a request takes, and a flag that is none.
    const std::vector<OpenBody::FrameSpec> seventeen(17, OpenBody::FrameSpec{ 0x100, 0, { 1 } });
    zassert_equal(
        unit.Write(Command::SEND, OpenBody().Put("tok", token).PutFrames(seventeen).Build()).Value("rc"),
        MGMT_ERR_EINVAL
    );
    const std::array<OpenBody::FrameSpec, 1> bad_flags = { OpenBody::FrameSpec{ 0x100, 0x10, { 1 } } };
    zassert_equal(
        unit.Write(Command::SEND, OpenBody().Put("tok", token).PutFrames(bad_flags).Build()).Value("rc"),
        MGMT_ERR_EINVAL
    );
    zassert_equal(
        unit.Write(Command::REPLAY, CborMap().Put("tok", token).Put("op", 9).Build()).Value("rc"), MGMT_ERR_EINVAL
    );
}

ZTEST(can_monitor_transmit, test_the_group_sets_periodic_frames) {
    // On the heap: the group keeps buffers for the lists it decodes, which is more than a test's stack likes.
    const auto unit_owner = std::make_unique<GroupUnit>();
    GroupUnit& unit = *unit_owner;
    const uint32_t token = OpenActiveThroughGroup(unit);
    ArmThroughGroup(unit, token);

    const std::array<OpenBody::PeriodicSpec, 1> periodic = { OpenBody::PeriodicSpec{
        1, { 0x321, 0, { 1 } }, 20, std::nullopt } };
    zassert_true(unit.Write(Command::PERIODIC, OpenBody().Put("tok", token).PutPeriodic(periodic).Build()).IsOk());
    zassert_true(WaitUntil([&] {
        return CountSent(unit.harness, 0x321) >= 2;
    }));

    const std::array<OpenBody::PeriodicSpec, 2> repeated = { OpenBody::PeriodicSpec{
                                                                 1, { 0x321, 0, { 1 } }, 20, std::nullopt },
        OpenBody::PeriodicSpec{ 1, { 0x322, 0, { 1 } }, 20, 2 } };
    zassert_equal(
        unit.Write(Command::PERIODIC, OpenBody().Put("tok", token).PutPeriodic(repeated).Build()).Value("rc"),
        MGMT_ERR_EINVAL
    );
    zassert_true(unit.Write(Command::PERIODIC, OpenBody().Put("tok", token).PutPeriodic({}).Build()).IsOk());
}

ZTEST(can_monitor_transmit, test_the_group_plays_a_replay) {
    // On the heap: the group keeps buffers for the lists it decodes, which is more than a test's stack likes.
    const auto unit_owner = std::make_unique<GroupUnit>();
    GroupUnit& unit = *unit_owner;
    const uint32_t token = OpenActiveThroughGroup(unit);
    ArmThroughGroup(unit, token);

    {
        const auto started = unit.Write(
            Command::REPLAY,
            CborMap().Put("tok", token).Put("op", 0).Put("speed_pct", 100).PutBool("loop", false).Build()
        );
        zassert_true(started.IsOk());
        zassert_equal(started.Value("room"), REPLAY_CAPACITY);
    }

    const Bytes records = ReplayRecords(SameFrames(0x500, 2, 1000));
    ExpectGroupError(
        unit.Write(
            Command::REPLAY,
            CborMap()
                .Put("tok", token)
                .Put("op", 1)
                .Put("seq", 3)
                .Put("data", std::span<const uint8_t>(records))
                .Build()
        ),
        CanMonitorError::BAD_SEQ
    );
    {
        const auto chunk = unit.Write(
            Command::REPLAY,
            CborMap()
                .Put("tok", token)
                .Put("op", 1)
                .Put("seq", 0)
                .Put("data", std::span<const uint8_t>(records))
                .Build()
        );
        zassert_true(chunk.IsOk());
        zassert_equal(chunk.Value("room"), REPLAY_CAPACITY - records.size());
    }
    zassert_true(unit.Write(Command::REPLAY, CborMap().Put("tok", token).Put("op", 3).Build()).IsOk());
    zassert_true(WaitUntil([&] {
        return CountSent(unit.harness, 0x500) == 2;
    }));
}

ZTEST(can_monitor_transmit, test_the_group_refuses_to_arm_a_session_with_no_bus) {
    // On the heap: the group keeps buffers for the lists it decodes, which is more than a test's stack likes.
    const auto unit_owner = std::make_unique<GroupUnit>();
    GroupUnit& unit = *unit_owner;

    const auto synthetic = unit.Write(Command::OPEN, OpenBody().PutSynthetic(10, 1, 1, false, false).Build());
    zassert_true(synthetic.IsOk());
    ExpectGroupError(
        unit.Write(Command::ARM, CborMap().Put("tok", synthetic.Value("tok")).PutBool("on", true).Build()),
        CanMonitorError::BAD_MODE
    );
}

#else // CONFIG_EERIE_LEAP_CAN_MONITOR_TX

ZTEST(can_monitor_transmit, test_without_transmit_the_commands_answer_unsupported) {
    // On the heap: the group keeps buffers for the lists it decodes, which is more than a test's stack likes.
    const auto unit_owner = std::make_unique<GroupUnit>();
    GroupUnit& unit = *unit_owner;

    const auto opened = unit.Write(Command::OPEN, OpenBody().Put("ch", CONFIGURED_PORT).Put("mode", 0).Build());
    zassert_true(opened.IsOk());
    const Bytes body = CborMap().Put("tok", opened.Value("tok")).Build();

    for(const Command command : { Command::ARM, Command::SEND, Command::PERIODIC, Command::REPLAY })
        ExpectGroupError(unit.Write(command, body), CanMonitorError::TX_UNSUPPORTED);

    // STATUS says so too, and the session is as it was.
    zassert_false(unit.harness.service->GetStatus().has_tx);
    zassert_equal(unit.harness.service->GetSessionCount(), 1U);
}

#endif // CONFIG_EERIE_LEAP_CAN_MONITOR_TX
