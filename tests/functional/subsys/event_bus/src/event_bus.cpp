#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <zephyr/ztest.h>

#include "subsys/event_bus/event_bus.h"

using eerie_leap::subsys::event_bus::Event;
using eerie_leap::subsys::event_bus::EventBus;
using eerie_leap::subsys::event_bus::SubscriptionHandle;

namespace {

enum class TestEventType : uint32_t {
    Alpha = 0,
    Beta = 1,
    Gamma = 2
};

enum class TestPayloadType : uint32_t {
    Value = 0,
    Label = 1
};

using TestEvent = Event<TestEventType, TestPayloadType>;
using TestEventBusBase = EventBus<TestEventType, TestPayloadType>;

// Subscribers are allowed to throw, and unwinding on 64 bit targets needs far
// more stack than dispatching does.
constexpr int STACK_SIZE = 8192;
constexpr int DISPATCH_TIMEOUT_MS = 1000;

// EventBus is only constructible through a derived bus, mirroring how applications specialize it.
class TestEventBus : public TestEventBusBase {
public:
    explicit TestEventBus(
        std::string name,
        DispatchGuardFn dispatch_guard_before = nullptr,
        DispatchGuardFn dispatch_guard_after = nullptr)
        : TestEventBusBase(std::move(name), STACK_SIZE, dispatch_guard_before, dispatch_guard_after) {}
};

atomic_t guard_before_calls;
atomic_t guard_after_calls;

void GuardBefore() { atomic_inc(&guard_before_calls); }
void GuardAfter() { atomic_inc(&guard_after_calls); }

int GuardBeforeCalls() { return static_cast<int>(atomic_get(&guard_before_calls)); }
int GuardAfterCalls() { return static_cast<int>(atomic_get(&guard_after_calls)); }

bool WaitForGuardAfterCalls(int expected) {
    int64_t deadline = k_uptime_get() + DISPATCH_TIMEOUT_MS;

    while(GuardAfterCalls() < expected && k_uptime_get() < deadline)
        k_msleep(1);

    return GuardAfterCalls() >= expected;
}

std::string CurrentThreadName() {
    const char* name = k_thread_name_get(k_current_get());

    return name != nullptr ? std::string(name) : std::string();
}

struct SourceFilter {
    std::string source_id;

    bool operator()(const TestEvent& event) const { return event.source_id == source_id; }
};

struct DispatchProbe {
    k_sem delivered;
    std::vector<int> values;
    std::string handler_thread_name;

    DispatchProbe() { k_sem_init(&delivered, 0, K_SEM_MAX_LIMIT); }

    void Record(const TestEvent& event) {
        handler_thread_name = CurrentThreadName();
        values.push_back(std::get<int>(event.payload.at(TestPayloadType::Value)));

        k_sem_give(&delivered);
    }

    bool WaitForDelivery() { return k_sem_take(&delivered, K_MSEC(DISPATCH_TIMEOUT_MS)) == 0; }
    [[nodiscard]] size_t Count() const { return values.size(); }
};

TestEvent MakeEvent(TestEventType type, int value = 0, std::string source_id = "test") {
    return TestEvent {
        .type = type,
        .payload = {{TestPayloadType::Value, value}},
        .source_id = std::move(source_id)
    };
}

void ResetGuardCounters(void*) {
    atomic_clear(&guard_before_calls);
    atomic_clear(&guard_after_calls);
}

} // namespace

ZTEST_SUITE(event_bus, NULL, NULL, ResetGuardCounters, NULL, NULL);

ZTEST(event_bus, test_Subscribe_returns_a_handle_bound_to_the_event_type) {
    TestEventBus bus("eb_subscribe");

    auto handle = bus.Subscribe(TestEventType::Alpha, [](const TestEvent&) {});

    zassert_true(handle.has_value());
    zassert_true(handle->IsValid());
    zassert_equal(handle->GetEventType(), TestEventType::Alpha);
}

ZTEST(event_bus, test_Subscribe_hands_out_a_distinct_id_per_subscription) {
    TestEventBus bus("eb_subscribe_ids");

    auto first = bus.Subscribe(TestEventType::Alpha, [](const TestEvent&) {});
    auto second = bus.Subscribe(TestEventType::Alpha, [](const TestEvent&) {});
    auto third = bus.Subscribe(TestEventType::Beta, [](const TestEvent&) {});

    zassert_true(first.has_value());
    zassert_true(second.has_value());
    zassert_true(third.has_value());
    zassert_not_equal(first->GetId(), second->GetId());
    zassert_not_equal(second->GetId(), third->GetId());
}

ZTEST(event_bus, test_Publish_delivers_the_event_on_the_calling_thread) {
    TestEventBus bus("eb_publish");
    DispatchProbe probe;

    auto handle = bus.Subscribe(TestEventType::Alpha, [&probe](const TestEvent& event) { probe.Record(event); });
    zassert_true(handle.has_value());

    bus.Publish(MakeEvent(TestEventType::Alpha, 42));

    zassert_equal(probe.Count(), 1U);
    zassert_equal(probe.values.front(), 42);
    zassert_equal(probe.handler_thread_name, CurrentThreadName());
}

ZTEST(event_bus, test_Publish_reaches_every_subscriber_of_the_event_type) {
    TestEventBus bus("eb_publish_fanout");
    DispatchProbe first;
    DispatchProbe second;

    auto first_handle = bus.Subscribe(TestEventType::Alpha, [&first](const TestEvent& event) { first.Record(event); });
    auto second_handle = bus.Subscribe(TestEventType::Alpha, [&second](const TestEvent& event) { second.Record(event); });
    zassert_true(first_handle.has_value());
    zassert_true(second_handle.has_value());

    bus.Publish(MakeEvent(TestEventType::Alpha, 7));

    zassert_equal(first.Count(), 1U);
    zassert_equal(second.Count(), 1U);
}

ZTEST(event_bus, test_Publish_ignores_subscribers_of_another_event_type) {
    TestEventBus bus("eb_publish_typed");
    DispatchProbe alpha;
    DispatchProbe beta;

    auto alpha_handle = bus.Subscribe(TestEventType::Alpha, [&alpha](const TestEvent& event) { alpha.Record(event); });
    auto beta_handle = bus.Subscribe(TestEventType::Beta, [&beta](const TestEvent& event) { beta.Record(event); });
    zassert_true(alpha_handle.has_value());
    zassert_true(beta_handle.has_value());

    bus.Publish(MakeEvent(TestEventType::Alpha, 1));

    zassert_equal(alpha.Count(), 1U);
    zassert_equal(beta.Count(), 0U);
}

ZTEST(event_bus, test_Publish_skips_subscribers_whose_filter_rejects_the_event) {
    TestEventBus bus("eb_publish_filtered");
    DispatchProbe probe;

    auto handle = bus.Subscribe(
        TestEventType::Alpha,
        SourceFilter{"sensor_1"},
        [&probe](const TestEvent& event) { probe.Record(event); });
    zassert_true(handle.has_value());

    bus.Publish(MakeEvent(TestEventType::Alpha, 1, "sensor_2"));

    zassert_equal(probe.Count(), 0U);

    bus.Publish(MakeEvent(TestEventType::Alpha, 2, "sensor_1"));

    zassert_equal(probe.Count(), 1U);
    zassert_equal(probe.values.front(), 2);
}

ZTEST(event_bus, test_Publish_without_subscribers_is_a_no_op) {
    TestEventBus bus("eb_publish_empty");

    bus.Publish(MakeEvent(TestEventType::Gamma, 1));
}

ZTEST(event_bus, test_Unsubscribe_stops_the_delivery_and_invalidates_the_handle) {
    TestEventBus bus("eb_unsubscribe");
    DispatchProbe probe;

    auto handle = bus.Subscribe(TestEventType::Alpha, [&probe](const TestEvent& event) { probe.Record(event); });
    zassert_true(handle.has_value());

    zassert_true(bus.Unsubscribe(handle.value()));
    zassert_false(handle->IsValid());

    bus.Publish(MakeEvent(TestEventType::Alpha, 1));

    zassert_equal(probe.Count(), 0U);
}

ZTEST(event_bus, test_Unsubscribe_keeps_the_remaining_subscribers_of_the_event_type) {
    TestEventBus bus("eb_unsubscribe_partial");
    DispatchProbe removed;
    DispatchProbe kept;

    auto removed_handle = bus.Subscribe(TestEventType::Alpha, [&removed](const TestEvent& event) { removed.Record(event); });
    auto kept_handle = bus.Subscribe(TestEventType::Alpha, [&kept](const TestEvent& event) { kept.Record(event); });
    zassert_true(removed_handle.has_value());
    zassert_true(kept_handle.has_value());

    zassert_true(bus.Unsubscribe(removed_handle.value()));

    bus.Publish(MakeEvent(TestEventType::Alpha, 1));

    zassert_equal(removed.Count(), 0U);
    zassert_equal(kept.Count(), 1U);
}

ZTEST(event_bus, test_Unsubscribe_reports_false_for_an_already_released_handle) {
    TestEventBus bus("eb_unsubscribe_twice");

    auto handle = bus.Subscribe(TestEventType::Alpha, [](const TestEvent&) {});
    zassert_true(handle.has_value());

    zassert_true(bus.Unsubscribe(handle.value()));
    zassert_false(bus.Unsubscribe(handle.value()));
}

ZTEST(event_bus, test_Unsubscribe_reports_false_for_an_unknown_subscription) {
    TestEventBus bus("eb_unsubscribe_unknown");

    auto handle = bus.Subscribe(TestEventType::Alpha, [](const TestEvent&) {});
    zassert_true(handle.has_value());

    SubscriptionHandle<TestEventType> unknown_event_type(handle->GetId(), TestEventType::Gamma);
    SubscriptionHandle<TestEventType> unknown_id(handle->GetId() + 1000, TestEventType::Alpha);

    zassert_false(bus.Unsubscribe(unknown_event_type));
    zassert_false(bus.Unsubscribe(unknown_id));
}

ZTEST(event_bus, test_a_throwing_subscriber_does_not_escape_Publish) {
    TestEventBus bus("eb_throwing");
    DispatchProbe probe;

    auto throwing_handle = bus.Subscribe(TestEventType::Alpha, [](const TestEvent&) {
        throw std::runtime_error("subscriber failed");
    });
    auto probe_handle = bus.Subscribe(TestEventType::Beta, [&probe](const TestEvent& event) { probe.Record(event); });
    zassert_true(throwing_handle.has_value());
    zassert_true(probe_handle.has_value());

    bus.Publish(MakeEvent(TestEventType::Alpha, 1));

    // The bus stays usable after a subscriber failure.
    bus.Publish(MakeEvent(TestEventType::Beta, 2));

    zassert_equal(probe.Count(), 1U);
    zassert_equal(probe.values.front(), 2);
}

ZTEST(event_bus, test_the_dispatch_guards_wrap_every_publication) {
    TestEventBus bus("eb_guards", GuardBefore, GuardAfter);
    DispatchProbe probe;

    auto handle = bus.Subscribe(TestEventType::Alpha, [&probe](const TestEvent& event) { probe.Record(event); });
    zassert_true(handle.has_value());

    bus.Publish(MakeEvent(TestEventType::Alpha, 1));
    bus.Publish(MakeEvent(TestEventType::Alpha, 2));

    zassert_equal(GuardBeforeCalls(), 2);
    zassert_equal(GuardAfterCalls(), 2);
    zassert_equal(probe.Count(), 2U);
}

ZTEST(event_bus, test_the_dispatch_guards_run_even_without_a_matching_subscriber) {
    TestEventBus bus("eb_guards_unmatched", GuardBefore, GuardAfter);

    bus.Publish(MakeEvent(TestEventType::Gamma, 1));

    zassert_equal(GuardBeforeCalls(), 1);
    zassert_equal(GuardAfterCalls(), 1);
}

ZTEST(event_bus, test_the_after_dispatch_guard_runs_when_a_subscriber_throws) {
    TestEventBus bus("eb_guards_throwing", GuardBefore, GuardAfter);

    auto handle = bus.Subscribe(TestEventType::Alpha, [](const TestEvent&) {
        throw std::runtime_error("subscriber failed");
    });
    zassert_true(handle.has_value());

    bus.Publish(MakeEvent(TestEventType::Alpha, 1));

    zassert_equal(GuardBeforeCalls(), 1);
    zassert_equal(GuardAfterCalls(), 1);
}

ZTEST(event_bus, test_PublishAsync_delivers_the_event_on_the_bus_thread) {
    TestEventBus bus("eb_async");
    DispatchProbe probe;

    auto handle = bus.Subscribe(TestEventType::Alpha, [&probe](const TestEvent& event) { probe.Record(event); });
    zassert_true(handle.has_value());

    bus.PublishAsync(MakeEvent(TestEventType::Alpha, 99));

    zassert_true(probe.WaitForDelivery(), "the event was never dispatched");
    zassert_equal(probe.Count(), 1U);
    zassert_equal(probe.values.front(), 99);
    zassert_equal(probe.handler_thread_name, std::string("eb_async"));
}

ZTEST(event_bus, test_PublishAsync_preserves_the_publication_order) {
    TestEventBus bus("eb_async_order");
    DispatchProbe probe;

    auto handle = bus.Subscribe(TestEventType::Alpha, [&probe](const TestEvent& event) { probe.Record(event); });
    zassert_true(handle.has_value());

    constexpr int event_count = 5;
    for(int i = 0; i < event_count; ++i)
        bus.PublishAsync(MakeEvent(TestEventType::Alpha, i));

    for(int i = 0; i < event_count; ++i)
        zassert_true(probe.WaitForDelivery(), "event %d was never dispatched", i);

    zassert_equal(probe.Count(), static_cast<size_t>(event_count));
    for(int i = 0; i < event_count; ++i)
        zassert_equal(probe.values[i], i);
}

ZTEST(event_bus, test_PublishAsync_applies_the_filters_and_the_dispatch_guards) {
    TestEventBus bus("eb_async_filtered", GuardBefore, GuardAfter);
    DispatchProbe probe;
    DispatchProbe accepted;

    auto filtered_handle = bus.Subscribe(
        TestEventType::Alpha,
        SourceFilter{"sensor_1"},
        [&probe](const TestEvent& event) { probe.Record(event); });
    auto accepted_handle = bus.Subscribe(TestEventType::Alpha, [&accepted](const TestEvent& event) { accepted.Record(event); });
    zassert_true(filtered_handle.has_value());
    zassert_true(accepted_handle.has_value());

    bus.PublishAsync(MakeEvent(TestEventType::Alpha, 1, "sensor_2"));

    zassert_true(accepted.WaitForDelivery(), "the event was never dispatched");
    zassert_true(WaitForGuardAfterCalls(1), "the after-dispatch guard never ran");
    zassert_equal(probe.Count(), 0U);
    zassert_equal(GuardBeforeCalls(), 1);
    zassert_equal(GuardAfterCalls(), 1);
}

ZTEST(event_bus, test_PublishAsync_stops_delivering_to_an_unsubscribed_handler) {
    TestEventBus bus("eb_async_unsubscribed");
    DispatchProbe removed;
    DispatchProbe kept;

    auto removed_handle = bus.Subscribe(TestEventType::Alpha, [&removed](const TestEvent& event) { removed.Record(event); });
    auto kept_handle = bus.Subscribe(TestEventType::Alpha, [&kept](const TestEvent& event) { kept.Record(event); });
    zassert_true(removed_handle.has_value());
    zassert_true(kept_handle.has_value());

    zassert_true(bus.Unsubscribe(removed_handle.value()));

    bus.PublishAsync(MakeEvent(TestEventType::Alpha, 1));

    zassert_true(kept.WaitForDelivery(), "the event was never dispatched");
    zassert_equal(removed.Count(), 0U);
}

ZTEST(event_bus, test_a_throwing_subscriber_does_not_stop_the_async_dispatch_loop) {
    TestEventBus bus("eb_async_throwing");
    DispatchProbe probe;

    auto throwing_handle = bus.Subscribe(TestEventType::Alpha, [](const TestEvent&) {
        throw std::runtime_error("subscriber failed");
    });
    auto probe_handle = bus.Subscribe(TestEventType::Beta, [&probe](const TestEvent& event) { probe.Record(event); });
    zassert_true(throwing_handle.has_value());
    zassert_true(probe_handle.has_value());

    bus.PublishAsync(MakeEvent(TestEventType::Alpha, 1));
    bus.PublishAsync(MakeEvent(TestEventType::Beta, 2));

    zassert_true(probe.WaitForDelivery(), "the queue stalled on the failing subscriber");
    zassert_equal(probe.values.front(), 2);
}
