#include <utility>
#include <vector>

#include <zephyr/ztest.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>

#include "smp_test_client.h"
#include "live_data_test_support.h"
#include "domain/ble_domain/smp/live_mgmt_group.h"

using namespace live_data_test;
using namespace smp_test;
using eerie_leap::domain::ble_domain::smp::LiveMgmtGroup;

using Command = LiveMgmtGroup::Command;
using Error = LiveMgmtGroup::Error;

namespace {

constexpr uint8_t Id(Command command) {
    return std::to_underlying(command);
}

struct GroupHarness {
    std::shared_ptr<FakeNotifier> notifier = std::make_shared<FakeNotifier>();
    Readings readings;
    std::shared_ptr<LiveDataService> service = std::make_shared<LiveDataService>(notifier, readings.frame);
    LiveMgmtGroup group{service};
    SmpTestClient client;

    GroupHarness() {
        notifier->on_notified = [service = service.get()]() { service->OnNotificationSent(); };
        group.Register();
    }

    Response Subscribe(const std::vector<uint32_t>& hashes, uint32_t period_ms) {
        return client.Write(SmpGroupId::LIVE, Id(Command::SUBSCRIBE),
            CborMap().Put("ids", std::span<const uint32_t>(hashes)).Put("period", period_ms).Build());
    }
};

void ExpectGroupError(const Response& response, Error error) {
    zassert_true(response.error.has_value(), "Expected a group error");
    zassert_equal(response.error_group.value(), std::to_underlying(SmpGroupId::LIVE));
    zassert_equal(response.error.value(), std::to_underlying(error));
}

} // namespace

ZTEST_SUITE(smp_live_group, NULL, NULL, NULL, NULL, NULL);

ZTEST(smp_live_group, test_subscribe_reports_the_period_and_capacity) {
    GroupHarness harness;
    const uint32_t hash = harness.readings.Add("sensor", 2.5F);

    const auto response = harness.Subscribe({hash}, 20);

    zassert_true(response.IsOk());
    zassert_equal(response.Value("period"), LiveDataService::MIN_PERIOD_MS);
    zassert_equal(response.Value("max"), 47);
    zassert_true(harness.service->IsSubscribed());

    const auto notifications = harness.notifier->WaitForNotifications(1);
    zassert_equal(notifications.size(), 1);
    zassert_equal(Decode(notifications[0]).values.size(), 1);
}

ZTEST(smp_live_group, test_too_many_sensors_report_the_capacity) {
    GroupHarness harness;

    harness.notifier->SetMaxSize(20);
    auto response = harness.Subscribe({1, 2, 3}, 100);
    ExpectGroupError(response, Error::TOO_MANY);
    zassert_equal(response.Value("max"), 2);

    // More than the service can hold at all.
    harness.notifier->SetMaxSize(1000);
    response = harness.Subscribe(std::vector<uint32_t>(LiveDataService::MAX_SENSORS + 1, 7), 100);
    ExpectGroupError(response, Error::TOO_MANY);
    zassert_equal(response.Value("max"), LiveDataService::MAX_SENSORS);

    zassert_false(harness.service->IsSubscribed());
}

ZTEST(smp_live_group, test_central_must_listen_on_live) {
    GroupHarness harness;
    harness.notifier->SetMaxSize(0);

    ExpectGroupError(harness.Subscribe({1}, 100), Error::NOT_LISTENING);
}

ZTEST(smp_live_group, test_incomplete_requests_are_invalid) {
    GroupHarness harness;

    auto response = harness.client.Write(SmpGroupId::LIVE, Id(Command::SUBSCRIBE), CborMap().Put("period", 100).Build());
    zassert_equal(response.Value("rc"), MGMT_ERR_EINVAL);

    const std::vector<uint32_t> hashes = {1};
    response = harness.client.Write(SmpGroupId::LIVE, Id(Command::SUBSCRIBE),
        CborMap().Put("ids", std::span<const uint32_t>(hashes)).Build());
    zassert_equal(response.Value("rc"), MGMT_ERR_EINVAL);

    zassert_equal(harness.Subscribe({}, 100).Value("rc"), MGMT_ERR_EINVAL);
}

ZTEST(smp_live_group, test_unsubscribe_stops_the_samples) {
    GroupHarness harness;
    zassert_true(harness.Subscribe({harness.readings.Add("sensor", 1.0F)}, 50).IsOk());

    const auto response = harness.client.Write(SmpGroupId::LIVE, Id(Command::UNSUBSCRIBE), CborMap().Build());

    zassert_true(response.IsOk());
    zassert_false(harness.service->IsSubscribed());
}
