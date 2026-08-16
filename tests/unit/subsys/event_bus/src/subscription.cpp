#include <cstddef>
#include <string>

#include <zephyr/ztest.h>

#include "subsys/event_bus/event.h"
#include "subsys/event_bus/event_filter.h"
#include "subsys/event_bus/subscription.h"

#include "test_events.h"

using eerie_leap::subsys::event_bus::AcceptAllFilter;
using eerie_leap::subsys::event_bus::Event;
using eerie_leap::subsys::event_bus::Subscription;

using event_bus_tests::TestEventType;
using event_bus_tests::TestPayloadType;

namespace {

using TestEvent = Event<TestEventType, TestPayloadType>;
using TestSubscription = Subscription<TestEventType, TestPayloadType>;

struct SourceFilter {
    std::string source_id;

    bool operator()(const TestEvent& event) const { return event.source_id == source_id; }
};

TestEvent MakeEvent(TestEventType type, std::string source_id) {
    return TestEvent {
        .type = type,
        .payload = {},
        .source_id = std::move(source_id)
    };
}

} // namespace

ZTEST_SUITE(event_bus_subscription, NULL, NULL, NULL, NULL, NULL);

ZTEST(event_bus_subscription, test_a_subscription_keeps_its_id_and_event_type) {
    TestSubscription subscription(
        7,
        TestEventType::Beta,
        AcceptAllFilter<TestEventType, TestPayloadType>{},
        [](const TestEvent&) {});

    zassert_equal(subscription.id, static_cast<size_t>(7));
    zassert_equal(subscription.event_type, TestEventType::Beta);
}

ZTEST(event_bus_subscription, test_a_subscription_erases_the_filter_type_but_keeps_its_behaviour) {
    TestSubscription subscription(
        1,
        TestEventType::Alpha,
        SourceFilter{"sensor_1"},
        [](const TestEvent&) {});

    zassert_true(subscription.filter(MakeEvent(TestEventType::Alpha, "sensor_1")));
    zassert_false(subscription.filter(MakeEvent(TestEventType::Alpha, "sensor_2")));
}

ZTEST(event_bus_subscription, test_a_subscription_invokes_the_handler_with_the_event) {
    int calls = 0;
    std::string observed_source;

    TestSubscription subscription(
        1,
        TestEventType::Alpha,
        AcceptAllFilter<TestEventType, TestPayloadType>{},
        [&calls, &observed_source](const TestEvent& event) {
            ++calls;
            observed_source = event.source_id;
        });

    subscription.handler(MakeEvent(TestEventType::Alpha, "sensor_3"));

    zassert_equal(calls, 1);
    zassert_equal(observed_source, std::string("sensor_3"));
}
