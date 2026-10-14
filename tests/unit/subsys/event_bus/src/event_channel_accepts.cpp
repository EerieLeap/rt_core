#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>

#include <zephyr/ztest.h>

#include "subsys/event_bus/erased_payload_view.h"
#include "subsys/event_bus/event.h"
#include "subsys/event_bus/event_channel.h"
#include "subsys/event_bus/i_event_bus.h"
#include "subsys/event_bus/subscription_handle.h"

#include "test_events.h"

using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::event_bus::CreateScopedSubscription;
using eerie_leap::subsys::event_bus::ErasedPayloadView;
using eerie_leap::subsys::event_bus::EventChannel;
using eerie_leap::subsys::event_bus::EventData;
using eerie_leap::subsys::event_bus::IEventBus;
using eerie_leap::subsys::event_bus::SubscriptionHandle;

using event_bus_tests::TestEventType;
using event_bus_tests::TestPayloadType;

namespace {

// No bus thread runs here: nothing is registered except through OnRegistered(), and DrainOne()
// runs on the test thread. A short ring keeps the 64-bit targets' heap use small.
class TestChannel : public EventChannel<TestEventType, TestPayloadType> {
public:
    explicit TestChannel(std::string name) : EventChannel(std::move(name), 4) { }
};

class CountingBus : public IEventBus {
public:
    int wakes = 0;

    void Wake() override { ++wakes; }
};

using TestEvent = TestChannel::EventMessage;

constexpr uint32_t k_source = 0xACCE;
constexpr uint32_t k_other_source = 0xBEEF;

TestEvent MakeEvent(TestEventType type, uint32_t value, uint32_t source_id = k_source) {
    return TestEvent {
        .source_id = source_id,
        .type = type,
        .payload = { { TestPayloadType::Value, value } }
    };
}

bool ValueIs(const TestEvent& event, uint32_t expected) {
    auto it = event.payload.find(TestPayloadType::Value);

    return it != event.payload.end()
        && std::holds_alternative<uint32_t>(it->second)
        && std::get<uint32_t>(it->second) == expected;
}

} // namespace

ZTEST_SUITE(event_bus_event_channel_accepts, NULL, NULL, NULL, NULL, NULL);

ZTEST(event_bus_event_channel_accepts, test_Accepts_is_false_without_a_subscriber_of_the_event_type) {
    TestChannel channel("accepts_none");

    zassert_false(channel.Accepts(MakeEvent(TestEventType::Alpha, 1)));

    AnySubscription subscription = CreateScopedSubscription(
        channel, TestEventType::Beta, [](const TestEvent&) { });
    zassert_not_null(subscription.get());

    // Another type's subscriber says nothing about this one.
    zassert_false(channel.Accepts(MakeEvent(TestEventType::Alpha, 1)));
    zassert_true(channel.Accepts(MakeEvent(TestEventType::Beta, 1)));
}

ZTEST(event_bus_event_channel_accepts, test_Accepts_runs_the_filters_of_the_event_type) {
    TestChannel channel("accepts_filter");

    AnySubscription subscription = CreateScopedSubscription(
        channel,
        TestEventType::Alpha,
        [](const TestEvent& event) { return ValueIs(event, 1); },
        [](const TestEvent&) { });

    zassert_true(channel.Accepts(MakeEvent(TestEventType::Alpha, 1)));
    zassert_false(channel.Accepts(MakeEvent(TestEventType::Alpha, 2)));
}

ZTEST(event_bus_event_channel_accepts, test_a_subscriber_without_a_filter_accepts_every_event) {
    TestChannel channel("accepts_unfiltered");

    auto typed = channel.Subscribe(TestEventType::Alpha, [](const TestEvent&) { });
    zassert_true(typed.has_value());
    zassert_true(channel.Accepts(MakeEvent(TestEventType::Alpha, 7)));
    zassert_true(channel.Unsubscribe(*typed));

    zassert_false(channel.Accepts(MakeEvent(TestEventType::Alpha, 7)));

    AnySubscription erased = channel.SubscribeErased(
        std::to_underlying(TestEventType::Alpha), { }, [](const ErasedPayloadView&) { });
    zassert_not_null(erased.get());
    zassert_true(channel.Accepts(MakeEvent(TestEventType::Alpha, 7)));
    zassert_true(channel.Accepts(TestEvent { .source_id = k_source, .type = TestEventType::Alpha, .payload = { } }));
}

ZTEST(event_bus_event_channel_accepts, test_Accepts_is_true_when_any_subscriber_accepts) {
    TestChannel channel("accepts_any");

    AnySubscription first = CreateScopedSubscription(
        channel,
        TestEventType::Alpha,
        [](const TestEvent& event) { return ValueIs(event, 1); },
        [](const TestEvent&) { });
    AnySubscription second = CreateScopedSubscription(
        channel,
        TestEventType::Alpha,
        [](const TestEvent& event) { return ValueIs(event, 2); },
        [](const TestEvent&) { });

    zassert_true(channel.Accepts(MakeEvent(TestEventType::Alpha, 1)));
    zassert_true(channel.Accepts(MakeEvent(TestEventType::Alpha, 2)));
    zassert_false(channel.Accepts(MakeEvent(TestEventType::Alpha, 3)));
}

ZTEST(event_bus_event_channel_accepts, test_Accepts_queues_nothing_and_calls_no_handler) {
    TestChannel channel("accepts_quiet");
    CountingBus bus;
    int handled = 0;

    channel.OnRegistered(&bus);

    {
        AnySubscription subscription = CreateScopedSubscription(
            channel, TestEventType::Alpha, [&handled](const TestEvent&) { ++handled; });

        zassert_true(channel.Accepts(MakeEvent(TestEventType::Alpha, 1)));
        zassert_equal(handled, 0);
        zassert_equal(bus.wakes, 0);
        zassert_false(channel.DrainOne(), "Accepts() must not queue the event");

        // The control: a publish does queue, wake and deliver.
        channel.PublishAsync(MakeEvent(TestEventType::Alpha, 1));
        zassert_equal(bus.wakes, 1);
        zassert_true(channel.DrainOne());
        zassert_equal(handled, 1);
    }

    channel.OnRegistered(nullptr);
}

ZTEST(event_bus_event_channel_accepts, test_Accepts_is_false_again_once_the_last_subscriber_leaves) {
    TestChannel channel("accepts_leave");

    AnySubscription subscription = CreateScopedSubscription(
        channel, TestEventType::Alpha, [](const TestEvent&) { });
    zassert_true(channel.Accepts(MakeEvent(TestEventType::Alpha, 1)));

    subscription.reset();

    zassert_false(channel.Accepts(MakeEvent(TestEventType::Alpha, 1)));
}

ZTEST(event_bus_event_channel_accepts, test_the_erased_Accepts_builds_the_event_it_would_publish) {
    TestChannel channel("accepts_erased");

    // Like a binding selector: the erased filter looks up one payload key.
    AnySubscription selector = channel.SubscribeErased(
        std::to_underlying(TestEventType::Alpha),
        [](const ErasedPayloadView& view) {
            const auto* label = view.Find(std::to_underlying(TestPayloadType::Label));

            return label != nullptr && std::holds_alternative<uint32_t>(*label) && std::get<uint32_t>(*label) == 42U;
        },
        [](const ErasedPayloadView&) { });
    zassert_not_null(selector.get());

    const std::array<std::pair<uint32_t, EventData>, 2> matching = {{
        { std::to_underlying(TestPayloadType::Label), EventData { 42U } },
        { std::to_underlying(TestPayloadType::Value), EventData { 1.5F } }
    }};
    const std::array<std::pair<uint32_t, EventData>, 1> other = {{
        { std::to_underlying(TestPayloadType::Label), EventData { 43U } }
    }};

    zassert_true(channel.Accepts(std::to_underlying(TestEventType::Alpha), k_source, matching));
    zassert_false(channel.Accepts(std::to_underlying(TestEventType::Alpha), k_source, other));
    zassert_false(channel.Accepts(std::to_underlying(TestEventType::Beta), k_source, matching));

    // A typed filter may select on the publisher, so the erased form carries the source too.
    AnySubscription by_source = CreateScopedSubscription(
        channel,
        TestEventType::Beta,
        [](const TestEvent& event) { return event.source_id == k_source; },
        [](const TestEvent&) { });

    zassert_true(channel.Accepts(std::to_underlying(TestEventType::Beta), k_source, other));
    zassert_false(channel.Accepts(std::to_underlying(TestEventType::Beta), k_other_source, other));
}

ZTEST(event_bus_event_channel_accepts, test_the_generation_moves_on_every_subscribe_and_unsubscribe) {
    TestChannel channel("generation_moves");

    uint32_t generation = channel.GetSubscriptionGeneration();

    auto typed = channel.Subscribe(TestEventType::Alpha, [](const TestEvent&) { });
    zassert_true(typed.has_value());
    zassert_not_equal(channel.GetSubscriptionGeneration(), generation, "a typed subscribe must move it");
    generation = channel.GetSubscriptionGeneration();

    AnySubscription erased = channel.SubscribeErased(
        std::to_underlying(TestEventType::Beta), { }, [](const ErasedPayloadView&) { });
    zassert_not_null(erased.get());
    zassert_not_equal(channel.GetSubscriptionGeneration(), generation, "an erased subscribe must move it");
    generation = channel.GetSubscriptionGeneration();

    zassert_true(channel.Unsubscribe(*typed));
    zassert_not_equal(channel.GetSubscriptionGeneration(), generation, "an unsubscribe must move it");
    generation = channel.GetSubscriptionGeneration();

    erased.reset();
    zassert_not_equal(channel.GetSubscriptionGeneration(), generation, "destroying a scoped subscription must move it");
}

ZTEST(event_bus_event_channel_accepts, test_the_generation_stays_on_publish_accepts_and_a_failed_unsubscribe) {
    TestChannel channel("generation_stays");

    // Stays, so a stale Alpha handle is looked for in a list that is not empty.
    AnySubscription staying = CreateScopedSubscription(
        channel, TestEventType::Alpha, [](const TestEvent&) { });
    zassert_not_null(staying.get());

    auto typed = channel.Subscribe(TestEventType::Alpha, [](const TestEvent&) { });
    zassert_true(typed.has_value());
    const size_t id = typed->GetId();
    zassert_true(channel.Unsubscribe(*typed));

    const uint32_t generation = channel.GetSubscriptionGeneration();

    (void)channel.Accepts(MakeEvent(TestEventType::Alpha, 1));
    channel.PublishAsync(MakeEvent(TestEventType::Alpha, 1));

    // Returns before the lock.
    zassert_false(channel.Unsubscribe(*typed), "the handle is already invalidated");

    // Valid handles that find nothing under the lock.
    SubscriptionHandle<TestEventType> stale { id, TestEventType::Alpha };
    zassert_false(channel.Unsubscribe(stale), "its subscription is already gone");
    SubscriptionHandle<TestEventType> unsubscribed_type { id, TestEventType::Gamma };
    zassert_false(channel.Unsubscribe(unsubscribed_type), "nobody subscribes to Gamma");

    zassert_equal(channel.GetSubscriptionGeneration(), generation);
}
