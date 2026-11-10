#include <array>
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
using eerie_leap::subsys::canbus::CanId;

using Command = CanMonitorMgmtGroup::Command;

namespace {

constexpr SmpGroupId GROUP = SmpGroupId::CAN_MONITOR;

constexpr uint8_t Id(Command command) {
    return std::to_underlying(command);
}

// The unit, its group and a client; the client goes first, then the group, then the unit.
struct GroupHarness {
    Harness harness;
    CanMonitorMgmtGroup group{ harness.service };
    SmpTestClient client;

    GroupHarness() {
        group.Register();
    }

    Response Open(const Bytes& body) {
        return client.Write(GROUP, Id(Command::OPEN), body);
    }

    Response OpenTap(uint32_t channel = CONFIGURED_PORT, uint32_t mode = 0) {
        return Open(OpenBody().Put("ch", channel).Put("mode", mode).Build());
    }

    Response Close(uint32_t token) {
        return client.Write(GROUP, Id(Command::CLOSE), CborMap().Put("tok", token).Build());
    }
};

void ExpectGroupError(const Response& response, CanMonitorError error) {
    zassert_true(response.error.has_value(), "Expected a group error");
    zassert_equal(response.error_group.value(), std::to_underlying(GROUP));
    zassert_equal(response.error.value(), std::to_underlying(error));
}

} // namespace

ZTEST_SUITE(can_monitor_group, NULL, NULL, NULL, ResetControllers, NULL);

ZTEST(can_monitor_group, test_open_and_close_answer_with_the_session) {
    GroupHarness unit;

    const auto opened = unit.OpenTap();
    zassert_true(opened.IsOk());
    zassert_not_equal(opened.Value("tok"), 0U);
    zassert_equal(opened.Value("ch"), CONFIGURED_PORT);
    zassert_equal(opened.Value("mode"), 0U);
    zassert_equal(opened.Value("type"), 1U, "A classical channel");
    zassert_equal(opened.Value("bitrate"), 500000U);
    zassert_equal(opened.Value("data_bitrate"), 0U);
    zassert_equal(opened.Value("lease_ms"), static_cast<uint32_t>(CONFIG_EERIE_LEAP_CAN_MONITOR_LEASE_MS));
    zassert_equal(unit.harness.service->GetSessionCount(), 1U);
    zassert_equal(unit.harness.holds.load(), 1);

    const auto closed = unit.Close(opened.Value("tok"));
    zassert_true(closed.IsOk());
    zassert_equal(unit.harness.service->GetSessionCount(), 0U);
    zassert_equal(unit.harness.releases.load(), 1);

    // The port is free again.
    zassert_true(unit.OpenTap().IsOk());
}

ZTEST(can_monitor_group, test_a_synthetic_session_states_port_255_and_streams) {
    GroupHarness unit;

    // `ch` and `mode` are ignored with `synthetic`.
    const auto opened = unit.Open(OpenBody().Put("ch", 7).Put("mode", 2).PutSynthetic(100, 2, 4, false, false).Build());
    zassert_true(opened.IsOk());
    zassert_equal(opened.Value("ch"), CAN_MONITOR_SYNTHETIC_PORT);
    zassert_equal(opened.Value("mode"), 0U);
    zassert_equal(opened.Value("type"), 0U);
    zassert_equal(opened.Value("bitrate"), 0U);
    zassert_equal(opened.Value("data_bitrate"), 0U);

    zassert_true(WaitUntil([&] {
        return unit.harness.notifier->Count() >= 1;
    }));
    const auto notifications = DecodeAll(unit.harness.notifier->Notifications());
    zassert_equal(notifications[0].port, CAN_SYNTHETIC_PORT);

    zassert_true(unit.Close(opened.Value("tok")).IsOk());
    zassert_true(WaitUntil([&] {
        unit.harness.service->OnNotificationSent();
        return unit.harness.HasEnded();
    }));
}

ZTEST(can_monitor_group, test_a_port_with_a_session_is_busy) {
    GroupHarness unit;
    const auto opened = unit.OpenTap();
    zassert_true(opened.IsOk());

    // Another request for the port is not the same OPEN again.
    const auto busy = unit.Open(OpenBody().Put("ch", CONFIGURED_PORT).Put("mode", 0).Put("type", 2).Build());
    ExpectGroupError(busy, CanMonitorError::BUSY);
    zassert_equal(unit.harness.service->GetSessionCount(), 1U);
}

ZTEST(can_monitor_group, test_a_lost_answer_gets_the_same_token_again) {
    GroupHarness unit;

    const auto first = unit.OpenTap();
    const auto again = unit.OpenTap();

    zassert_true(first.IsOk());
    zassert_true(again.IsOk());
    zassert_equal(again.Value("tok"), first.Value("tok"));
    zassert_equal(unit.harness.service->GetSessionCount(), 1U, "No second session");
    zassert_equal(unit.harness.holds.load(), 1, "Held once");

    // The same holds for a synthetic session.
    const Bytes synthetic = OpenBody().PutSynthetic(50, 2, 2, false, false).Build();
    const auto synthetic_first = unit.Open(synthetic);
    const auto synthetic_again = unit.Open(synthetic);
    zassert_true(synthetic_first.IsOk());
    zassert_equal(synthetic_again.Value("tok"), synthetic_first.Value("tok"));
    zassert_not_equal(synthetic_first.Value("tok"), first.Value("tok"));
    zassert_equal(unit.harness.service->GetSessionCount(), 2U);
}

ZTEST(can_monitor_group, test_unknown_ports_and_modes_the_port_cannot_take) {
    GroupHarness unit;

    // A port the devicetree has no alias for.
    ExpectGroupError(unit.OpenTap(5), CanMonitorError::NOT_FOUND);

    // TAP is for a channel the configuration runs; can1 is not run.
    ExpectGroupError(unit.OpenTap(UNCONFIGURED_PORT), CanMonitorError::BAD_MODE);

    // LISTEN_ONLY and ACTIVE are for a port the configuration doesn't run; can0 is run. And a mode nobody knows.
    ExpectGroupError(unit.OpenTap(CONFIGURED_PORT, 1), CanMonitorError::BAD_MODE);
    ExpectGroupError(unit.OpenTap(CONFIGURED_PORT, 2), CanMonitorError::BAD_MODE);
    ExpectGroupError(unit.OpenTap(CONFIGURED_PORT, 7), CanMonitorError::BAD_MODE);
    ExpectGroupError(unit.OpenTap(UNCONFIGURED_PORT, 7), CanMonitorError::BAD_MODE);

    zassert_equal(unit.harness.service->GetSessionCount(), 0U);
    zassert_equal(unit.harness.holds.load(), 0);
}

ZTEST(can_monitor_group, test_open_needs_a_central_listening_on_bus) {
    GroupHarness unit;
    unit.harness.notifier->SetMaxSize(0);

    ExpectGroupError(unit.OpenTap(), CanMonitorError::NOT_LISTENING);
    ExpectGroupError(
        unit.Open(OpenBody().PutSynthetic(10, 1, 1, false, false).Build()), CanMonitorError::NOT_LISTENING
    );
    zassert_equal(unit.harness.service->GetSessionCount(), 0U);
}

ZTEST(can_monitor_group, test_close_names_what_is_wrong_with_the_token) {
    GroupHarness unit;
    const auto opened = unit.OpenTap();
    const uint32_t token = opened.Value("tok");

    ExpectGroupError(unit.Close(0), CanMonitorError::BAD_TOKEN);
    ExpectGroupError(unit.Close(token + 1), CanMonitorError::NO_SESSION);
    zassert_equal(unit.harness.service->GetSessionCount(), 1U, "Nothing was closed");

    zassert_true(unit.Close(token).IsOk());
    ExpectGroupError(unit.Close(token), CanMonitorError::NO_SESSION);

    // A request with no token at all is malformed.
    zassert_equal(unit.client.Write(GROUP, Id(Command::CLOSE), CborMap().Build()).Value("rc"), MGMT_ERR_EINVAL);
}

ZTEST(can_monitor_group, test_malformed_requests_are_invalid) {
    GroupHarness unit;

    // No port, no mode.
    zassert_equal(unit.Open(OpenBody().Put("mode", 0).Build()).Value("rc"), MGMT_ERR_EINVAL);
    zassert_equal(unit.Open(OpenBody().Put("ch", 0).Build()).Value("rc"), MGMT_ERR_EINVAL);
    // A channel that is no byte; a type nobody has.
    zassert_equal(unit.Open(OpenBody().Put("ch", 300).Put("mode", 0).Build()).Value("rc"), MGMT_ERR_EINVAL);
    zassert_equal(
        unit.Open(OpenBody().Put("ch", 0).Put("mode", 0).Put("type", 9).Build()).Value("rc"), MGMT_ERR_EINVAL
    );
    // Synthetic parameters that can't be generated, or are missing.
    zassert_equal(unit.Open(OpenBody().PutSynthetic(0, 2, 4, false, false).Build()).Value("rc"), MGMT_ERR_EINVAL);
    zassert_equal(unit.Open(OpenBody().PutSynthetic(10, 2, 9, false, false).Build()).Value("rc"), MGMT_ERR_EINVAL);

    // More filters than a session takes.
    std::vector<OpenBody::FilterSpec> filters(CONFIG_EERIE_LEAP_CAN_MONITOR_MAX_FILTERS + 1);
    zassert_equal(
        unit.Open(OpenBody().Put("ch", 0).Put("mode", 0).PutFilters(filters).Build()).Value("rc"), MGMT_ERR_EINVAL
    );

    zassert_equal(unit.harness.service->GetSessionCount(), 0U);
}

ZTEST(can_monitor_group, test_filters_of_open_are_applied_to_the_stream) {
    GroupHarness unit;
    Canbus& bus = *unit.harness.Bus();

    constexpr std::array<OpenBody::FilterSpec, 2> FILTERS = {
        OpenBody::FilterSpec{ 0x200, 0x7FF, false },
        OpenBody::FilterSpec{ 0x18DA0000, 0x1FFF0000, true },
    };
    const auto opened = unit.Open(OpenBody().Put("ch", 0).Put("mode", 0).PutFilters(FILTERS).Build());
    zassert_true(opened.IsOk());

    const std::array<uint8_t, 1> payload = { 0x42 };
    zassert_ok(bus.SendFrame(CanId::Standard(0x123), payload));
    zassert_ok(bus.SendFrame(CanId::Standard(0x200), payload));
    zassert_ok(bus.SendFrame(CanId::Extended(0x18DA1234), payload));
    // Extended with the same low bits as a filter's standard id: IDE decides.
    zassert_ok(bus.SendFrame(CanId::Extended(0x200), payload));

    // Two frames pass, each twice (received and echoed).
    zassert_true(WaitUntil([&] {
        const auto last = unit.harness.notifier->Notifications();
        size_t frames = 0;
        for(const SentNotification& sent : last) {
            const auto decoded = DecodeNotification(sent);
            frames += decoded ? RecordsOf({ *decoded }, CanRecordKind::FRAME).size() : 0;
        }
        return frames >= 4;
    }));
    k_msleep(100);
    unit.harness.CloseAndDrain(opened.Value("tok"));

    const auto notifications = DecodeAll(unit.harness.notifier->Notifications());
    const auto frames = RecordsOf(notifications, CanRecordKind::FRAME);
    zassert_equal(frames.size(), 4U);
    for(const DecodedRecord* frame : frames) {
        const bool passes = (!frame->record.ide && frame->record.id == 0x200)
            || (frame->record.ide && frame->record.id == 0x18DA1234);
        zassert_true(passes, "Frame 0x%x passed the filter", frame->record.id);
    }

    const CanStatistics sum = SumStatistics(notifications);
    zassert_equal(sum.filtered, 4U, "0x123 and the extended 0x200, each twice");
    zassert_equal(sum.frames_received, 4U);
    zassert_equal(sum.frames_sent, 4U);
}
