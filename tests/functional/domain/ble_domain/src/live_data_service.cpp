#include <array>
#include <cerrno>

#include <zephyr/ztest.h>

#include "live_data_test_support.h"

using namespace live_data_test;

using SubscribeResult = LiveDataService::SubscribeResult;

ZTEST_SUITE(live_data_service, NULL, NULL, NULL, NULL, NULL);

ZTEST(live_data_service, test_capacity_follows_the_notification_size) {
    const auto notifier = std::make_shared<FakeNotifier>();
    const Readings readings;
    LiveDataService service(notifier, readings.frame);

    notifier->SetMaxSize(244);
    zassert_equal(service.GetCapacity(), 47);
    notifier->SetMaxSize(20);
    zassert_equal(service.GetCapacity(), 2);
    notifier->SetMaxSize(1000);
    zassert_equal(service.GetCapacity(), LiveDataService::MAX_SENSORS);
    notifier->SetMaxSize(0);
    zassert_equal(service.GetCapacity(), 0);
}

ZTEST(live_data_service, test_subscription_is_limited_to_one_notification) {
    const auto notifier = std::make_shared<FakeNotifier>();
    const Readings readings;
    LiveDataService service(notifier, readings.frame);
    const std::array<uint32_t, 3> hashes = { 1, 2, 3 };

    notifier->SetMaxSize(0);
    zassert_equal(service.Subscribe(hashes, AllValues(hashes.size()), 100), SubscribeResult::NOT_LISTENING);

    notifier->SetMaxSize(20);
    zassert_equal(service.Subscribe(hashes, AllValues(hashes.size()), 100), SubscribeResult::TOO_MANY);
    zassert_false(service.IsSubscribed());

    zassert_equal(service.Subscribe(std::span(hashes).first(2), AllValues(2), 100), SubscribeResult::OK);
    zassert_true(service.IsSubscribed());
}

ZTEST(live_data_service, test_samples_carry_values_by_subscription_position) {
    const auto notifier = std::make_shared<FakeNotifier>();
    Readings readings;
    const uint32_t first = readings.Add("first", 1.5F);
    const uint32_t pending = readings.Add("pending", std::nullopt);
    const uint32_t last = readings.Add("last", 3.25F);
    LiveDataService service(notifier, readings.frame);

    const std::array<uint32_t, 4> hashes = { last, pending, 12345, first };
    const uint32_t before_ms = k_uptime_get_32();
    zassert_equal(service.Subscribe(hashes, AllValues(hashes.size()), 1000), SubscribeResult::OK);

    const auto notifications = notifier->WaitForNotifications(1);
    zassert_equal(notifications.size(), 1);

    const auto sample = Decode(notifications[0]);
    zassert_equal(sample.version, LiveDataEncoder::VERSION);
    zassert_equal(sample.sequence, 0);
    zassert_true(sample.time_ms >= before_ms && sample.time_ms <= k_uptime_get_32());
    zassert_equal(sample.values.size(), 2);
    zassert_equal(sample.values[0].first, 0);
    zassert_equal(sample.values[0].second, 3.25F);
    zassert_equal(sample.values[1].first, 3);
    zassert_equal(sample.values[1].second, 1.5F);
}

ZTEST(live_data_service, test_fields_must_match_the_sensors) {
    const auto notifier = std::make_shared<FakeNotifier>();
    const Readings readings;
    LiveDataService service(notifier, readings.frame);
    const std::array<uint32_t, 2> hashes = { 1, 2 };

    zassert_equal(service.Subscribe(hashes, AllValues(1), 100), SubscribeResult::TOO_MANY);
    zassert_false(service.IsSubscribed());
}

ZTEST(live_data_service, test_one_sensor_listed_for_each_field) {
    const auto notifier = std::make_shared<FakeNotifier>();
    Readings readings;
    const uint32_t analog = readings.Add("analog", 80.0F, 4.0F, 2.0F);
    const uint32_t plain = readings.Add("plain", 1.5F);
    LiveDataService service(notifier, readings.frame);

    const std::array<uint32_t, 4> hashes = { analog, analog, analog, plain };
    const std::array<ReadingField, 4> fields = { ReadingField::VALUE, ReadingField::CALIBRATED_VOLTAGE,
        ReadingField::ADC_VOLTAGE, ReadingField::ADC_VOLTAGE };
    zassert_equal(service.Subscribe(hashes, fields, 1000), SubscribeResult::OK);

    const auto notifications = notifier->WaitForNotifications(1);
    zassert_equal(notifications.size(), 1);

    // Each entry is one position; the plain sensor has no ADC voltage, so its entry is left out.
    const auto sample = Decode(notifications[0]);
    zassert_equal(sample.values.size(), 3);
    zassert_equal(sample.values[0].first, 0);
    zassert_equal(sample.values[0].second, 80.0F);
    zassert_equal(sample.values[1].first, 1);
    zassert_equal(sample.values[1].second, 4.0F);
    zassert_equal(sample.values[2].first, 2);
    zassert_equal(sample.values[2].second, 2.0F);
}

ZTEST(live_data_service, test_period_is_at_least_the_minimum) {
    zassert_equal(LiveDataService::ClampPeriod(10), LiveDataService::MIN_PERIOD_MS);
    zassert_equal(LiveDataService::ClampPeriod(200), 200);

    const auto notifier = std::make_shared<FakeNotifier>();
    Readings readings;
    LiveDataService service(notifier, readings.frame);
    notifier->on_notified = [&service] {
        service.OnNotificationSent();
    };

    const std::array<uint32_t, 1> hashes = { readings.Add("sensor", 1.0F) };
    zassert_equal(service.Subscribe(hashes, AllValues(hashes.size()), 10), SubscribeResult::OK);
    k_msleep(175);
    service.Unsubscribe();

    // 10 ms would give well over a dozen samples.
    const size_t count = notifier->Notifications().size();
    zassert_true(count >= 2 && count <= 5, "%u samples in 175 ms", static_cast<unsigned>(count));
}

ZTEST(live_data_service, test_one_notification_in_flight) {
    const auto notifier = std::make_shared<FakeNotifier>();
    Readings readings;
    LiveDataService service(notifier, readings.frame);

    const std::array<uint32_t, 1> hashes = { readings.Add("sensor", 1.0F) };
    zassert_equal(
        service.Subscribe(hashes, AllValues(hashes.size()), LiveDataService::MIN_PERIOD_MS), SubscribeResult::OK
    );
    k_msleep(200);

    zassert_equal(notifier->Notifications().size(), 1, "The first notification never reported back");
    zassert_true(service.GetDroppedCount() >= 2);

    service.OnNotificationSent();
    const auto notifications = notifier->WaitForNotifications(2);
    zassert_equal(notifications.size(), 2);

    // The sequence counts samples, so the dropped ones show as a gap.
    zassert_true(Decode(notifications[1]).sequence >= 3);
}

ZTEST(live_data_service, test_sample_without_a_buffer_is_dropped) {
    const auto notifier = std::make_shared<FakeNotifier>();
    Readings readings;
    LiveDataService service(notifier, readings.frame);
    notifier->SetResult(-ENOMEM);

    const std::array<uint32_t, 1> hashes = { readings.Add("sensor", 1.0F) };
    zassert_equal(
        service.Subscribe(hashes, AllValues(hashes.size()), LiveDataService::MIN_PERIOD_MS), SubscribeResult::OK
    );
    k_msleep(120);

    zassert_true(notifier->Notifications().empty());
    zassert_true(service.GetDroppedCount() >= 1);

    notifier->SetResult(0);
    zassert_equal(notifier->WaitForNotifications(1).size(), 1, "Nothing stays in flight after a failure");
}

ZTEST(live_data_service, test_unsubscribe_stops_sampling) {
    const auto notifier = std::make_shared<FakeNotifier>();
    Readings readings;
    LiveDataService service(notifier, readings.frame);
    notifier->on_notified = [&service] {
        service.OnNotificationSent();
    };

    const std::array<uint32_t, 1> hashes = { readings.Add("sensor", 1.0F) };
    zassert_equal(
        service.Subscribe(hashes, AllValues(hashes.size()), LiveDataService::MIN_PERIOD_MS), SubscribeResult::OK
    );
    zassert_equal(notifier->WaitForNotifications(1).size(), 1);

    service.Unsubscribe();
    notifier->Clear();
    k_msleep(150);

    zassert_true(notifier->Notifications().empty());
    zassert_false(service.IsSubscribed());
}
