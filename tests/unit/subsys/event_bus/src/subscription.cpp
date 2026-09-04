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

constexpr uint32_t k_source_one = 0xA001;
constexpr uint32_t k_source_two = 0xA002;
constexpr uint32_t k_source_three = 0xA003;

struct SourceFilter {
    uint32_t source_id;

    bool operator()(const TestEvent& event) const { return event.source_id == source_id; }
};

TestEvent MakeEvent(TestEventType type, uint32_t source_id) {
    return TestEvent {
        .source_id = source_id,
        .type = type,
        .payload = {}
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
        SourceFilter{k_source_one},
        [](const TestEvent&) {});

    zassert_true(subscription.filter(MakeEvent(TestEventType::Alpha, k_source_one)));
    zassert_false(subscription.filter(MakeEvent(TestEventType::Alpha, k_source_two)));
}

ZTEST(event_bus_subscription, test_a_subscription_invokes_the_handler_with_the_event) {
    int calls = 0;
    uint32_t observed_source = 0;

    TestSubscription subscription(
        1,
        TestEventType::Alpha,
        AcceptAllFilter<TestEventType, TestPayloadType>{},
        [&calls, &observed_source](const TestEvent& event) {
            ++calls;
            observed_source = event.source_id;
        });

    subscription.handler(MakeEvent(TestEventType::Alpha, k_source_three));

    zassert_equal(calls, 1);
    zassert_equal(observed_source, k_source_three);
}
