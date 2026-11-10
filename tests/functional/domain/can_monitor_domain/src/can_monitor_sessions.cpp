#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>
#include <zephyr/ztest.h>

#include "can_monitor_status_client.h"
#include "can_monitor_test_support.h"
#include "domain/can_monitor_domain/smp/can_monitor_mgmt_group.h"
#include "smp_test_client.h"

using namespace can_monitor_test;
using namespace smp_test;

using eerie_leap::domain::can_monitor_domain::smp::CanMonitorMgmtGroup;
using eerie_leap::subsys::canbus::CanId;

using Command = CanMonitorMgmtGroup::Command;

namespace {

constexpr SmpGroupId GROUP = SmpGroupId::CAN_MONITOR;
constexpr uint32_t PROBE_ID = 0x123;
constexpr uint32_t OTHER_ID = 0x200;
// 47 bit times of overhead and 3 data bytes at 500 kbit/s: 71 x 2 µs.
constexpr uint64_t BUSY_US_PER_FRAME = 142;

constexpr uint8_t Id(Command command) {
    return std::to_underlying(command);
}

CanMonitorOpenAnswer OpenTap(Harness& harness, std::span<const CanUnitFilter> filters = {}) {
    CanMonitorOpenRequest request;
    request.ch = CONFIGURED_PORT;
    request.mode = CanMonitorMode::TAP;
    request.filters = filters;

    const auto answer = harness.service->Open(request);
    zassert_true(answer.has_value(), "TAP OPEN failed (%d)", answer ? 0 : static_cast<int>(answer.error()));

    return *answer;
}

std::expected<CanMonitorOpenAnswer, CanMonitorError> OpenOwn(
    Harness& harness,
    CanMonitorMode mode,
    std::optional<uint32_t> bitrate,
    std::optional<CanbusType> type = std::nullopt,
    std::optional<uint32_t> data_bitrate = std::nullopt
) {
    CanMonitorOpenRequest request;
    request.ch = UNCONFIGURED_PORT;
    request.mode = mode;
    request.bitrate = bitrate;
    request.type = type;
    request.data_bitrate = data_bitrate;

    return harness.service->Open(request);
}

// The frames with this identifier the stream holds so far, received (tx false) or echoed (tx true).
size_t CountFrames(Harness& harness, uint32_t id, bool is_tx) {
    size_t count = 0;
    WalkStream(harness.notifier->Notifications(), [&](const CanRecord& record, uint64_t) {
        if(record.kind == CanRecordKind::FRAME && record.id == id && record.tx == is_tx)
            ++count;
    });

    return count;
}

std::array<uint8_t, 3> PayloadOf(uint8_t index) {
    return { index, static_cast<uint8_t>(0xA0 + index), 0x55 };
}

void Send(Canbus& bus, uint32_t id, uint8_t index) {
    zassert_ok(bus.SendFrame(CanId::Standard(id), PayloadOf(index)));
}

void ExpectGroupError(const Response& response, CanMonitorError error) {
    zassert_true(response.error.has_value(), "Expected a group error");
    zassert_equal(response.error_group.value(), std::to_underlying(GROUP));
    zassert_equal(response.error.value(), std::to_underlying(error));
}

// The unit, its group and a client for each kind of answer; the clients go first, then the group, then the unit.
struct GroupUnit {
    Harness harness;
    CanMonitorMgmtGroup group{ harness.service };
    SmpTestClient client;
    RawSmpClient raw_client;

    GroupUnit() {
        group.Register();
    }

    StatusView Status() {
        const auto body = raw_client.Read(GROUP, Id(Command::STATUS));
        zassert_true(body.has_value(), "No answer to STATUS");

        const auto view = DecodeStatus(*body);
        zassert_true(view.has_value(), "STATUS does not decode");

        return *view;
    }

    Response Filter(uint32_t token, std::span<const OpenBody::FilterSpec> filters) {
        // OpenBody writes the filters under their key; the token goes beside them.
        return client.Write(GROUP, Id(Command::FILTER), OpenBody().Put("tok", token).PutFilters(filters).Build());
    }

    Response Stats(uint32_t token) {
        return client.Read(GROUP, Id(Command::STATS), CborMap().Put("tok", token).Build());
    }
};

} // namespace

ZTEST_SUITE(can_monitor_sessions, NULL, NULL, NULL, ResetControllers, NULL);

ZTEST(can_monitor_sessions, test_status_lists_the_ports_and_what_the_build_has) {
    GroupUnit unit;

    auto status = unit.Status();
    zassert_equal(status.version, 1U);
    zassert_equal(status.ports.size(), 2U, "can0 and can1; the synthetic port is not listed");
    zassert_equal(status.filters_max, static_cast<uint32_t>(CONFIG_EERIE_LEAP_CAN_MONITOR_MAX_FILTERS));
    zassert_equal(status.ring_bytes, static_cast<uint32_t>(CONFIG_EERIE_LEAP_CAN_MONITOR_RING_SIZE));
    zassert_equal(status.notify_max, NOTIFY_MAX);
    zassert_false(status.has_tx);
    zassert_true(status.has_synthetic);
    zassert_true(status.has_delta);

    const StatusPort* can0 = status.Port(CONFIGURED_PORT);
    zassert_not_null(can0);
    zassert_equal(can0->types, (std::vector<uint32_t>{ 1, 2 }), "Classical and CAN FD");
    zassert_true(can0->is_configured);
    zassert_equal(can0->state, 0U, "Error active");
    zassert_equal(can0->bitrate, 500000U);
    zassert_equal(can0->data_bitrate, 0U);
    zassert_false(can0->has_session);

    const StatusPort* can1 = status.Port(UNCONFIGURED_PORT);
    zassert_not_null(can1);
    zassert_false(can1->is_configured);
    zassert_equal(can1->state, 4U, "Stopped");
    zassert_equal(can1->bitrate, 0U);
    zassert_false(can1->has_session);

    // A session holds its port; an own bus gives the port its bitrate.
    const auto tap = OpenTap(unit.harness);
    const auto own = OpenOwn(unit.harness, CanMonitorMode::LISTEN_ONLY, 250000);
    zassert_true(own.has_value());

    status = unit.Status();
    zassert_true(status.Port(CONFIGURED_PORT)->has_session);
    zassert_true(status.Port(UNCONFIGURED_PORT)->has_session);
    zassert_false(status.Port(UNCONFIGURED_PORT)->is_configured);
    zassert_equal(status.Port(UNCONFIGURED_PORT)->bitrate, 250000U);
    zassert_equal(status.Port(UNCONFIGURED_PORT)->state, 0U);

    // The largest notification the link takes now, and never more than the sender's buffer.
    unit.harness.notifier->SetMaxSize(0);
    zassert_equal(unit.Status().notify_max, 0U);
    unit.harness.notifier->SetMaxSize(1000);
    zassert_equal(unit.Status().notify_max, static_cast<uint32_t>(CanMonitorSender::MAX_NOTIFICATION_SIZE));

    zassert_true(unit.harness.service->Close(tap.tok).has_value());
    zassert_true(unit.harness.service->Close(own->tok).has_value());
}

ZTEST(can_monitor_sessions, test_filter_replaces_the_unit_filters_of_a_session) {
    GroupUnit unit;
    Canbus& bus = *unit.harness.Bus();
    const auto tap = OpenTap(unit.harness);

    // Only 0x200 passes.
    const std::array<OpenBody::FilterSpec, 1> only_other = { OpenBody::FilterSpec{ OTHER_ID, 0x7FF, false } };
    zassert_true(unit.Filter(tap.tok, only_other).IsOk());

    Send(bus, PROBE_ID, 0);
    Send(bus, OTHER_ID, 1);
    zassert_true(WaitUntil([&] {
        return CountFrames(unit.harness, OTHER_ID, false) == 1 && CountFrames(unit.harness, OTHER_ID, true) == 1;
    }));
    k_msleep(100);
    zassert_equal(CountFrames(unit.harness, PROBE_ID, false), 0U);
    zassert_equal(CountFrames(unit.harness, PROBE_ID, true), 0U);

    // An empty list passes every frame.
    zassert_true(unit.Filter(tap.tok, {}).IsOk());
    Send(bus, PROBE_ID, 2);
    zassert_true(WaitUntil([&] {
        return CountFrames(unit.harness, PROBE_ID, false) == 1 && CountFrames(unit.harness, PROBE_ID, true) == 1;
    }));

    // More than a session takes is invalid, and the filters stay as they were.
    const std::vector<OpenBody::FilterSpec> too_many(CONFIG_EERIE_LEAP_CAN_MONITOR_MAX_FILTERS + 1);
    zassert_equal(unit.Filter(tap.tok, too_many).Value("rc"), MGMT_ERR_EINVAL);
    Send(bus, PROBE_ID, 3);
    zassert_true(WaitUntil([&] {
        return CountFrames(unit.harness, PROBE_ID, false) == 2;
    }));

    // The token is checked as everywhere.
    ExpectGroupError(unit.Filter(0, {}), CanMonitorError::BAD_TOKEN);
    ExpectGroupError(unit.Filter(tap.tok + 1, {}), CanMonitorError::NO_SESSION);
    zassert_equal(
        unit.client.Write(GROUP, Id(Command::FILTER), CborMap().Put("tok", tap.tok).Build()).Value("rc"),
        MGMT_ERR_EINVAL, "No filters key"
    );
}

ZTEST(can_monitor_sessions, test_stats_hold_the_totals_since_open_with_the_controllers_counts) {
    GroupUnit unit;
    Canbus& bus = *unit.harness.Bus();
    const auto tap = OpenTap(unit.harness);

    Send(bus, PROBE_ID, 0);
    Send(bus, PROBE_ID, 1);
    zassert_true(WaitUntil([&] {
        return CountFrames(unit.harness, PROBE_ID, false) == 2 && CountFrames(unit.harness, PROBE_ID, true) == 2;
    }));

    // Errors and an overrun the controller counted. The overrun reaches the stream as a gap with the next frame.
    unit.harness.counters->AddError(0, 3);
    unit.harness.counters->AddError(4, 2);
    unit.harness.counters->AddOverruns(2);
    Send(bus, PROBE_ID, 2);
    zassert_true(WaitUntil([&] {
        return CountFrames(unit.harness, PROBE_ID, false) == 3 && CountFrames(unit.harness, PROBE_ID, true) == 3;
    }));

    const auto stats = unit.Stats(tap.tok);
    zassert_true(stats.IsOk());
    zassert_equal(stats.Value("state"), 0U);
    zassert_equal(stats.Value("tec"), 0U);
    zassert_equal(stats.Value("rec"), 0U);
    zassert_equal(stats.Value("rx"), 3U);
    zassert_equal(stats.Value("tx"), 3U);
    zassert_equal(stats.Value("busy_us"), 6 * BUSY_US_PER_FRAME);
    zassert_equal(stats.Value("filtered"), 0U);
    zassert_equal(stats.uint_lists.at("errors"), (std::vector<uint32_t>{ 3, 0, 0, 0, 2 }));
    zassert_equal(stats.uint_lists.at("lost"), (std::vector<uint32_t>{ 2, 0, 0, 0 }));

    // Statistics records carry the counts since the previous record; they add up to the same.
    unit.harness.CloseAndDrain(tap.tok);
    const auto notifications = DecodeAll(unit.harness.notifier->Notifications());
    const CanStatistics sum = SumStatistics(notifications);
    zassert_equal(sum.errors[CanStatistics::ERROR_BIT], 3U);
    zassert_equal(sum.errors[CanStatistics::ERROR_ACK], 2U);
    zassert_equal(sum.errors[CanStatistics::ERROR_CRC], 0U);
    zassert_equal(sum.lost[CanStatistics::LOST_CONTROLLER], 2U);
    zassert_equal(sum.frames_received, 3U);

    // The overruns are one source: the gap says the same.
    uint64_t overrun_gap_frames = 0;
    for(const DecodedRecord* gap : RecordsOf(notifications, CanRecordKind::GAP)) {
        if(gap->record.gap_cause == CanGapCause::CONTROLLER)
            overrun_gap_frames += gap->record.frames_lost;
    }
    zassert_equal(overrun_gap_frames, 2U);
}

ZTEST(can_monitor_sessions, test_stats_names_what_is_wrong_with_the_token) {
    GroupUnit unit;
    const auto opened = unit.harness.OpenSynthetic(20, 2, 2);

    ExpectGroupError(unit.Stats(0), CanMonitorError::BAD_TOKEN);
    ExpectGroupError(unit.Stats(opened.tok + 1), CanMonitorError::NO_SESSION);
    zassert_equal(unit.client.Read(GROUP, Id(Command::STATS)).Value("rc"), MGMT_ERR_EINVAL);

    // A session with no bus has no controller: error active, no counters.
    const auto stats = unit.Stats(opened.tok);
    zassert_true(stats.IsOk());
    zassert_equal(stats.Value("state"), 0U);
    zassert_equal(stats.Value("tec"), 0U);
    zassert_equal(stats.Value("busy_us"), 0U);
    zassert_equal(stats.uint_lists.at("errors"), (std::vector<uint32_t>{ 0, 0, 0, 0, 0 }));
}

ZTEST(can_monitor_sessions, test_a_lapsed_lease_ends_the_session_with_its_end_record) {
    Harness harness;
    const auto answer = harness.OpenSynthetic(50, 2, 2);
    zassert_equal(answer.lease_ms, static_cast<uint32_t>(CONFIG_EERIE_LEAP_CAN_MONITOR_LEASE_MS));

    // Nothing carries the token: the lease runs out, with room for a slow runner.
    zassert_true(WaitUntil(
        [&] {
            return harness.service->GetSessionCount() == 0;
        },
        3 * CONFIG_EERIE_LEAP_CAN_MONITOR_LEASE_MS
    ));
    zassert_true(WaitUntil([&] {
        return harness.HasEnded();
    }));
    zassert_equal(harness.holds.load(), harness.releases.load());

    const StreamSummary summary = Summarize(harness.notifier->Notifications());
    zassert_equal(summary.ends.size(), 1U);
    zassert_equal(summary.ends[0], CanEndReason::LEASE);

    zassert_equal(harness.service->GetStats(answer.tok).error(), CanMonitorError::NO_SESSION);
}

ZTEST(can_monitor_sessions, test_requests_with_the_token_keep_the_lease_alive) {
    Harness harness;
    const auto answer = harness.OpenSynthetic(50, 2, 2);
    constexpr int LEASE_MS = CONFIG_EERIE_LEAP_CAN_MONITOR_LEASE_MS;

    // Longer than a lease, with a request a half second: STATS, then FILTER, then STATS again.
    for(int slept = 0; slept < 3 * LEASE_MS / 2; slept += 500) {
        const bool is_filter = (slept / 500) % 2 == 1;
        const bool is_ok = is_filter ? harness.service->SetFilters(answer.tok, {}).has_value()
                                     : harness.service->GetStats(answer.tok).has_value();
        zassert_true(is_ok, "Alive after %d ms", slept);

        k_msleep(500);
    }
    zassert_equal(harness.service->GetSessionCount(), 1U);

    // Then nobody asks.
    zassert_true(WaitUntil(
        [&] {
            return harness.service->GetSessionCount() == 0;
        },
        3 * LEASE_MS
    ));
    zassert_true(WaitUntil([&] {
        return harness.HasEnded();
    }));
    zassert_equal(Summarize(harness.notifier->Notifications()).ends[0], CanEndReason::LEASE);
}

ZTEST(can_monitor_sessions, test_the_lost_answer_grace_ends_when_a_request_uses_the_token) {
    Harness harness;

    CanMonitorOpenRequest request;
    request.ch = CONFIGURED_PORT;
    const auto first = harness.service->Open(request);
    const auto again = harness.service->Open(request);
    zassert_true(first.has_value() && again.has_value());
    zassert_equal(again->tok, first->tok);

    zassert_true(harness.service->GetStats(first->tok).has_value());

    // The client has the answer, so this is another client.
    zassert_equal(harness.service->Open(request).error(), CanMonitorError::BUSY);
}

ZTEST(can_monitor_sessions, test_a_reconfiguration_ends_every_session_and_frees_the_buses) {
    Harness harness;
    const auto tap = OpenTap(harness);
    const auto own = OpenOwn(harness, CanMonitorMode::ACTIVE, 500000);
    zassert_true(own.has_value());
    zassert_equal(harness.service->GetSessionCount(), 2U);

    // What CanbusController::Reconfigure() does around CanbusService::Configure().
    zassert_true(harness.service->Stop());
    zassert_equal(harness.service->GetSessionCount(), 0U);
    zassert_equal(harness.holds.load(), harness.releases.load());
    zassert_is_null(harness.service->GetOwnBus(own->tok));

    // Each session ends its own stream.
    for(const uint8_t port : { CONFIGURED_PORT, UNCONFIGURED_PORT }) {
        zassert_true(
            WaitUntil([&] {
                return Summarize(OnPort(harness.notifier->Notifications(), port)).ends.size() == 1;
            }),
            "The stream of port %u ends", port
        );

        const StreamSummary summary = Summarize(OnPort(harness.notifier->Notifications(), port));
        zassert_equal(summary.ends[0], CanEndReason::CONFIGURATION_STORED);
    }

    // The service takes OPEN again, and the port of the own bus is free.
    zassert_true(harness.service->Start());
    const auto again = OpenOwn(harness, CanMonitorMode::LISTEN_ONLY, 500000);
    zassert_true(again.has_value());
    zassert_not_equal(again->tok, tap.tok);
}

ZTEST(can_monitor_sessions, test_listen_only_on_an_unconfigured_port_builds_a_bus_streams_and_frees_it) {
    Harness harness;

    const auto answer = OpenOwn(harness, CanMonitorMode::LISTEN_ONLY, 500000);
    zassert_true(answer.has_value(), "OPEN failed (%d)", answer ? 0 : static_cast<int>(answer.error()));
    zassert_equal(answer->ch, UNCONFIGURED_PORT);
    zassert_equal(answer->mode, CanMonitorMode::LISTEN_ONLY);
    zassert_equal(answer->type, eerie_leap::subsys::canbus::CanbusType::CLASSICAL_CAN);
    zassert_equal(answer->bitrate, 500000U);
    zassert_equal(answer->data_bitrate, 0U);
    zassert_equal(harness.holds.load(), 1);

    // The loopback driver has no listen-only mode and Canbus leaves it out of what it applies, so the
    // session's bus is asked about the mode it was built for.
    Canbus* own_bus = harness.service->GetOwnBus(answer->tok);
    zassert_not_null(own_bus);
    const auto config = own_bus->GetConfig();
    zassert_equal(config.canbus_dev, UnconfiguredDevice());
    zassert_not_equal(config.extra_modes & CAN_MODE_LISTENONLY, 0U);
    zassert_equal(config.bitrate, 500000U);

    // A frame that the controller receives is in the stream.
    SwitchToLoopbackMode(UnconfiguredDevice());
    can_frame frame{};
    frame.id = 0x321;
    frame.dlc = 2;
    frame.data[0] = 0x11;
    frame.data[1] = 0x22;
    zassert_ok(can_send(UnconfiguredDevice(), &frame, K_MSEC(100), nullptr, nullptr));

    zassert_true(WaitUntil([&] {
        return CountFrames(harness, 0x321, false) == 1;
    }));

    harness.CloseAndDrain(answer->tok);
    zassert_is_null(harness.service->GetOwnBus(answer->tok));
    zassert_equal(harness.releases.load(), 1);

    const auto notifications = DecodeAll(harness.notifier->Notifications());
    for(const DecodedNotification& notification : notifications)
        zassert_equal(notification.port, UNCONFIGURED_PORT);

    const auto frames = RecordsOf(notifications, CanRecordKind::FRAME);
    zassert_equal(frames.size(), 1U);
    zassert_equal(frames[0]->record.id, 0x321U);
    zassert_equal(frames[0]->payload, (Bytes{ 0x11, 0x22 }));

    // The bus is freed: another session can build one on the port.
    const auto again = OpenOwn(harness, CanMonitorMode::ACTIVE, 500000);
    zassert_true(again.has_value());
    zassert_equal(harness.service->GetOwnBus(again->tok)->GetConfig().extra_modes & CAN_MODE_LISTENONLY, 0U);
}

ZTEST(can_monitor_sessions, test_active_on_an_unconfigured_port_sends_and_sees_its_own_frames) {
    Harness harness;

    const auto answer = OpenOwn(harness, CanMonitorMode::ACTIVE, 250000);
    zassert_true(answer.has_value());
    zassert_equal(answer->mode, CanMonitorMode::ACTIVE);
    zassert_equal(answer->bitrate, 250000U);

    SwitchToLoopbackMode(UnconfiguredDevice());
    Canbus* own_bus = harness.service->GetOwnBus(answer->tok);
    zassert_not_null(own_bus);
    Send(*own_bus, PROBE_ID, 7);

    // Received by the loopback and echoed as sent, like on a configured channel.
    zassert_true(WaitUntil([&] {
        return CountFrames(harness, PROBE_ID, false) == 1 && CountFrames(harness, PROBE_ID, true) == 1;
    }));

    // The bitrate of the session is the bus's: 250 kbit/s, 71 bit times of 4 µs for each frame.
    const auto stats = harness.service->GetStats(answer->tok);
    zassert_true(stats.has_value());
    zassert_equal(stats->frames_received, 1U);
    zassert_equal(stats->frames_sent, 1U);
    zassert_equal(stats->busy_us, 2 * 2 * BUSY_US_PER_FRAME);

    harness.CloseAndDrain(answer->tok);
}

ZTEST(can_monitor_sessions, test_a_port_bitrate_the_bus_cannot_run_is_refused) {
    Harness harness;

    const auto classical = OpenOwn(harness, CanMonitorMode::LISTEN_ONLY, 123456);
    zassert_false(classical.has_value());
    zassert_equal(classical.error(), CanMonitorError::BAD_BITRATE);

    const auto fd = OpenOwn(harness, CanMonitorMode::ACTIVE, 500000, CanbusType::CANFD, 999);
    zassert_false(fd.has_value());
    zassert_equal(fd.error(), CanMonitorError::BAD_BITRATE);

    // Nothing was left behind: the port opens at a good bitrate.
    zassert_equal(harness.service->GetSessionCount(), 0U);
    zassert_equal(harness.holds.load(), 0);
    zassert_true(OpenOwn(harness, CanMonitorMode::LISTEN_ONLY, 500000).has_value());
}

ZTEST(can_monitor_sessions, test_an_fd_port_takes_its_data_bitrate_or_uses_the_nominal_one) {
    Harness harness;

    const auto nominal = OpenOwn(harness, CanMonitorMode::LISTEN_ONLY, 500000, CanbusType::CANFD);
    zassert_true(nominal.has_value(), "OPEN failed (%d)", nominal ? 0 : static_cast<int>(nominal.error()));
    zassert_equal(nominal->type, CanbusType::CANFD);
    zassert_equal(nominal->bitrate, 500000U);
    zassert_equal(nominal->data_bitrate, 500000U, "The nominal bitrate when none is given");
    zassert_true(harness.service->Close(nominal->tok).has_value());

    const auto fast = OpenOwn(harness, CanMonitorMode::LISTEN_ONLY, 500000, CanbusType::CANFD, 1000000);
    zassert_true(fast.has_value());
    zassert_equal(fast->data_bitrate, 1000000U);
}

ZTEST(can_monitor_sessions, test_a_bus_that_detects_its_bitrate_answers_zero_and_closes_in_time) {
    Harness harness;

    // The loopback controller cannot be made to see traffic while the bus probes its bitrates (each probe
    // sets the mode again), so the detection itself is not run to its end here: the session is open on a bus
    // that is still looking, and CLOSE stops it.
    const auto answer = OpenOwn(harness, CanMonitorMode::LISTEN_ONLY, std::nullopt);
    zassert_true(answer.has_value(), "OPEN failed (%d)", answer ? 0 : static_cast<int>(answer.error()));
    zassert_equal(answer->bitrate, 0U);
    zassert_equal(answer->data_bitrate, 0U);

    k_msleep(1300);
    zassert_equal(harness.service->GetStatus().ports[UNCONFIGURED_PORT].bitrate, 0U);

    const int64_t closing_ms = k_uptime_get();
    harness.CloseAndDrain(answer->tok);
    zassert_true(k_uptime_get() - closing_ms < 6000, "Closing stops the detection");

    // No frame and no state came before a bitrate: the stream holds the statistics and the end only.
    const StreamSummary summary = Summarize(harness.notifier->Notifications());
    zassert_equal(summary.frames, 0U);
    zassert_equal(summary.ends.size(), 1U);

    zassert_true(OpenOwn(harness, CanMonitorMode::LISTEN_ONLY, 500000).has_value(), "The port is free");
}

ZTEST(can_monitor_sessions, test_a_bus_that_stopped_under_its_session_ends_it_with_an_error) {
    Harness harness;

    const auto answer = OpenOwn(harness, CanMonitorMode::ACTIVE, 500000);
    zassert_true(answer.has_value());
    zassert_true(harness.service->GetOwnBus(answer->tok)->Stop());

    zassert_true(WaitUntil(
        [&] {
            return harness.service->GetSessionCount() == 0;
        },
        5000
    ));
    zassert_true(WaitUntil([&] {
        return harness.HasEnded();
    }));

    const StreamSummary summary = Summarize(harness.notifier->Notifications());
    zassert_equal(summary.ends.size(), 1U);
    zassert_equal(summary.ends[0], CanEndReason::ERROR);
    zassert_equal(harness.holds.load(), harness.releases.load());
}

ZTEST(can_monitor_sessions, test_changed_bytes_decode_to_the_frames_that_were_generated) {
    Harness harness;
    const CanSyntheticParameters parameters{ .fps = 500, .ids = 4, .dlc = 8 };

    CanMonitorOpenRequest request;
    request.synthetic = parameters;
    request.delta = true;
    const auto answer = harness.service->Open(request);
    zassert_true(answer.has_value());

    harness.Sleep(answer->tok, 700);
    harness.CloseAndDrain(answer->tok);

    // The app's side of the table: the same rules, so it ends up with the same payloads.
    auto table = CanDeltaTable::Create();
    zassert_not_null(table.get());
    CanDeltaDecoder decoder(*table);

    size_t frames = 0;
    size_t changed = 0;
    WalkStream(harness.notifier->Notifications(), [&](const CanRecord& record, uint64_t) {
        std::array<uint8_t, 64> payload{};
        const auto length = decoder.Apply(record, payload);
        zassert_true(length.has_value(), "Frame %zu does not apply", frames);

        if(record.kind != CanRecordKind::FRAME && record.kind != CanRecordKind::CHANGED_BYTES)
            return;

        if(record.kind == CanRecordKind::CHANGED_BYTES)
            ++changed;

        zassert_equal(record.id, CanSyntheticSource::IdOf(parameters, frames));
        zassert_equal(*length, 8U);
        for(size_t k = 0; k < 8; ++k)
            zassert_equal(payload[k], CanSyntheticSource::PayloadByte(frames, k), "Frame %zu byte %zu", frames, k);

        ++frames;
    });

    zassert_true(frames > 100, "Frames: %zu", frames);
    zassert_true(changed > 0, "Repeats of an ID go as changed bytes");
    zassert_true(frames - changed >= 4, "The first frame of each ID goes whole");
}

ZTEST(can_monitor_sessions, test_changed_bytes_are_left_out_unless_open_asks) {
    Harness harness;

    const auto answer = harness.OpenSynthetic(300, 2, 8);
    harness.Sleep(answer.tok, 300);
    harness.CloseAndDrain(answer.tok);

    size_t changed = 0;
    WalkStream(harness.notifier->Notifications(), [&](const CanRecord& record, uint64_t) {
        if(record.kind == CanRecordKind::CHANGED_BYTES)
            ++changed;
    });
    zassert_equal(changed, 0U);
}
