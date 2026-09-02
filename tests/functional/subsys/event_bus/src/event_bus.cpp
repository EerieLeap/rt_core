#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <zephyr/ztest.h>

#include "subsys/event_bus/event_bus.h"
#include "subsys/event_bus/event_channel.h"
#include "subsys/event_bus/i_event_bus.h"
#include "subsys/event_bus/scoped_subscription.h"

using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::event_bus::CreateScopedSubscription;
using eerie_leap::subsys::event_bus::Event;
using eerie_leap::subsys::event_bus::EventBus;
using eerie_leap::subsys::event_bus::EventChannel;
using eerie_leap::subsys::event_bus::IEventBus;
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

enum class OtherEventType : uint32_t {
    Signal = 0
};

enum class OtherPayloadType : uint32_t {
    Value = 0
};

using TestEvent = Event<TestEventType, TestPayloadType>;

// Subscribers are allowed to throw, and unwinding on 64 bit targets needs far
// more stack than dispatching does.
constexpr int STACK_SIZE = 8192;
constexpr int DISPATCH_TIMEOUT_MS = 1000;

// Both constructors are protected, mirroring how applications specialize them.
class TestChannel : public EventChannel<TestEventType, TestPayloadType> {
public:
    explicit TestChannel(std::string name, size_t max_queued_events = 64)
        : EventChannel(std::move(name), max_queued_events) {}
};

class OtherChannel : public EventChannel<OtherEventType, OtherPayloadType> {
public:
    explicit OtherChannel(std::string name) : EventChannel(std::move(name), 16) {}
};

class TestEventBus : public EventBus {
public:
    explicit TestEventBus(std::string name) : EventBus(std::move(name), STACK_SIZE) {}
};

// Declaration order matters: the bus is destroyed first, so ~EventBus detaches a
// channel that is still alive. Reversing these two members dangles.
struct BusWithChannel {
    TestChannel channel;
    TestEventBus bus;

    explicit BusWithChannel(const std::string& name, size_t capacity = 64)
        : channel(name + "_ch", capacity), bus(name) {

        bus.RegisterChannel(channel);
    }
};

// Registers a channel without standing up a worker thread, so queued events stay
// queued until the test drains them by hand.
class ManualBus : public IEventBus {
public:
    void Wake() override { ++wake_calls; }

    int wake_calls = 0;
};

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

} // namespace

ZTEST_SUITE(event_bus, NULL, NULL, NULL, NULL, NULL);

ZTEST(event_bus, test_Subscribe_returns_a_handle_bound_to_the_event_type) {
    BusWithChannel fixture("eb_subscribe");

    auto handle = fixture.channel.Subscribe(TestEventType::Alpha, [](const TestEvent&) {});

    zassert_true(handle.has_value());
    zassert_true(handle->IsValid());
    zassert_equal(handle->GetEventType(), TestEventType::Alpha);
}

ZTEST(event_bus, test_Subscribe_hands_out_a_distinct_id_per_subscription) {
    BusWithChannel fixture("eb_subscribe_ids");

    auto first = fixture.channel.Subscribe(TestEventType::Alpha, [](const TestEvent&) {});
    auto second = fixture.channel.Subscribe(TestEventType::Alpha, [](const TestEvent&) {});
    auto third = fixture.channel.Subscribe(TestEventType::Beta, [](const TestEvent&) {});

    zassert_true(first.has_value());
    zassert_true(second.has_value());
    zassert_true(third.has_value());
    zassert_not_equal(first->GetId(), second->GetId());
    zassert_not_equal(second->GetId(), third->GetId());
}

ZTEST(event_bus, test_Publish_delivers_the_event_on_the_calling_thread) {
    BusWithChannel fixture("eb_publish");
    DispatchProbe probe;

    auto handle = fixture.channel.Subscribe(TestEventType::Alpha, [&probe](const TestEvent& event) { probe.Record(event); });
    zassert_true(handle.has_value());

    fixture.channel.Publish(MakeEvent(TestEventType::Alpha, 42));

    zassert_equal(probe.Count(), 1U);
    zassert_equal(probe.values.front(), 42);
    zassert_equal(probe.handler_thread_name, CurrentThreadName());
}

ZTEST(event_bus, test_Publish_reaches_every_subscriber_of_the_event_type) {
    BusWithChannel fixture("eb_publish_fanout");
    DispatchProbe first;
    DispatchProbe second;

    auto first_handle = fixture.channel.Subscribe(TestEventType::Alpha, [&first](const TestEvent& event) { first.Record(event); });
    auto second_handle = fixture.channel.Subscribe(TestEventType::Alpha, [&second](const TestEvent& event) { second.Record(event); });
    zassert_true(first_handle.has_value());
    zassert_true(second_handle.has_value());

    fixture.channel.Publish(MakeEvent(TestEventType::Alpha, 7));

    zassert_equal(first.Count(), 1U);
    zassert_equal(second.Count(), 1U);
}

ZTEST(event_bus, test_Publish_ignores_subscribers_of_another_event_type) {
    BusWithChannel fixture("eb_publish_typed");
    DispatchProbe alpha;
    DispatchProbe beta;

    auto alpha_handle = fixture.channel.Subscribe(TestEventType::Alpha, [&alpha](const TestEvent& event) { alpha.Record(event); });
    auto beta_handle = fixture.channel.Subscribe(TestEventType::Beta, [&beta](const TestEvent& event) { beta.Record(event); });
    zassert_true(alpha_handle.has_value());
    zassert_true(beta_handle.has_value());

    fixture.channel.Publish(MakeEvent(TestEventType::Alpha, 1));

    zassert_equal(alpha.Count(), 1U);
    zassert_equal(beta.Count(), 0U);
}

ZTEST(event_bus, test_Publish_skips_subscribers_whose_filter_rejects_the_event) {
    BusWithChannel fixture("eb_publish_filtered");
    DispatchProbe probe;

    auto handle = fixture.channel.Subscribe(
        TestEventType::Alpha,
        SourceFilter{"sensor_1"},
        [&probe](const TestEvent& event) { probe.Record(event); });
    zassert_true(handle.has_value());

    fixture.channel.Publish(MakeEvent(TestEventType::Alpha, 1, "sensor_2"));

    zassert_equal(probe.Count(), 0U);

    fixture.channel.Publish(MakeEvent(TestEventType::Alpha, 2, "sensor_1"));

    zassert_equal(probe.Count(), 1U);
    zassert_equal(probe.values.front(), 2);
}

ZTEST(event_bus, test_Publish_without_subscribers_is_a_no_op) {
    BusWithChannel fixture("eb_publish_empty");

    fixture.channel.Publish(MakeEvent(TestEventType::Gamma, 1));
}

ZTEST(event_bus, test_Unsubscribe_stops_the_delivery_and_invalidates_the_handle) {
    BusWithChannel fixture("eb_unsubscribe");
    DispatchProbe probe;

    auto handle = fixture.channel.Subscribe(TestEventType::Alpha, [&probe](const TestEvent& event) { probe.Record(event); });
    zassert_true(handle.has_value());

    zassert_true(fixture.channel.Unsubscribe(handle.value()));
    zassert_false(handle->IsValid());

    fixture.channel.Publish(MakeEvent(TestEventType::Alpha, 1));

    zassert_equal(probe.Count(), 0U);
}

ZTEST(event_bus, test_Unsubscribe_keeps_the_remaining_subscribers_of_the_event_type) {
    BusWithChannel fixture("eb_unsubscribe_partial");
    DispatchProbe removed;
    DispatchProbe kept;

    auto removed_handle = fixture.channel.Subscribe(TestEventType::Alpha, [&removed](const TestEvent& event) { removed.Record(event); });
    auto kept_handle = fixture.channel.Subscribe(TestEventType::Alpha, [&kept](const TestEvent& event) { kept.Record(event); });
    zassert_true(removed_handle.has_value());
    zassert_true(kept_handle.has_value());

    zassert_true(fixture.channel.Unsubscribe(removed_handle.value()));

    fixture.channel.Publish(MakeEvent(TestEventType::Alpha, 1));

    zassert_equal(removed.Count(), 0U);
    zassert_equal(kept.Count(), 1U);
}

ZTEST(event_bus, test_Unsubscribe_reports_false_for_an_already_released_handle) {
    BusWithChannel fixture("eb_unsubscribe_twice");

    auto handle = fixture.channel.Subscribe(TestEventType::Alpha, [](const TestEvent&) {});
    zassert_true(handle.has_value());

    zassert_true(fixture.channel.Unsubscribe(handle.value()));
    zassert_false(fixture.channel.Unsubscribe(handle.value()));
}

ZTEST(event_bus, test_Unsubscribe_reports_false_for_an_unknown_subscription) {
    BusWithChannel fixture("eb_unsubscribe_unknown");

    auto handle = fixture.channel.Subscribe(TestEventType::Alpha, [](const TestEvent&) {});
    zassert_true(handle.has_value());

    SubscriptionHandle<TestEventType> unknown_event_type(handle->GetId(), TestEventType::Gamma);
    SubscriptionHandle<TestEventType> unknown_id(handle->GetId() + 1000, TestEventType::Alpha);

    zassert_false(fixture.channel.Unsubscribe(unknown_event_type));
    zassert_false(fixture.channel.Unsubscribe(unknown_id));
}

ZTEST(event_bus, test_a_subscriber_may_unsubscribe_itself_during_dispatch) {
    BusWithChannel fixture("eb_unsub_self");
    DispatchProbe self_probe;
    DispatchProbe peer_probe;

    std::optional<SubscriptionHandle<TestEventType>> self_handle;

    auto subscription = fixture.channel.Subscribe(TestEventType::Alpha, [&](const TestEvent& event) {
        self_probe.Record(event);

        if(self_handle.has_value())
            fixture.channel.Unsubscribe(self_handle.value());
    });
    zassert_true(subscription.has_value());
    self_handle.emplace(std::move(*subscription));

    auto peer_handle = fixture.channel.Subscribe(TestEventType::Alpha, [&peer_probe](const TestEvent& event) { peer_probe.Record(event); });
    zassert_true(peer_handle.has_value());

    fixture.channel.Publish(MakeEvent(TestEventType::Alpha, 1));

    // Dispatch runs off a snapshot, so removing a subscription cannot invalidate it.
    zassert_equal(self_probe.Count(), 1U);
    zassert_equal(peer_probe.Count(), 1U);

    fixture.channel.Publish(MakeEvent(TestEventType::Alpha, 2));

    zassert_equal(self_probe.Count(), 1U);
    zassert_equal(peer_probe.Count(), 2U);
}

ZTEST(event_bus, test_a_subscriber_removed_during_dispatch_still_receives_the_in_flight_event) {
    BusWithChannel fixture("eb_unsub_peer");
    DispatchProbe first_probe;
    DispatchProbe removed_probe;

    std::optional<SubscriptionHandle<TestEventType>> removed_handle;

    auto first_handle = fixture.channel.Subscribe(TestEventType::Alpha, [&](const TestEvent& event) {
        first_probe.Record(event);

        if(removed_handle.has_value())
            fixture.channel.Unsubscribe(removed_handle.value());
    });
    zassert_true(first_handle.has_value());

    auto subscription = fixture.channel.Subscribe(TestEventType::Alpha, [&removed_probe](const TestEvent& event) { removed_probe.Record(event); });
    zassert_true(subscription.has_value());
    removed_handle.emplace(std::move(*subscription));

    fixture.channel.Publish(MakeEvent(TestEventType::Alpha, 1));

    zassert_equal(first_probe.Count(), 1U);
    zassert_equal(removed_probe.Count(), 1U);

    fixture.channel.Publish(MakeEvent(TestEventType::Alpha, 2));

    zassert_equal(first_probe.Count(), 2U);
    zassert_equal(removed_probe.Count(), 1U);
}

ZTEST(event_bus, test_a_subscriber_added_during_dispatch_starts_at_the_next_event) {
    BusWithChannel fixture("eb_sub_in_dispatch");
    DispatchProbe late_probe;
    std::vector<SubscriptionHandle<TestEventType>> late_handles;

    auto handle = fixture.channel.Subscribe(TestEventType::Alpha, [&](const TestEvent&) {
        if(!late_handles.empty())
            return;

        auto late = fixture.channel.Subscribe(TestEventType::Alpha, [&late_probe](const TestEvent& event) { late_probe.Record(event); });
        if(late)
            late_handles.push_back(std::move(*late));
    });
    zassert_true(handle.has_value());

    fixture.channel.Publish(MakeEvent(TestEventType::Alpha, 1));

    zassert_equal(late_handles.size(), 1U);
    zassert_equal(late_probe.Count(), 0U);

    fixture.channel.Publish(MakeEvent(TestEventType::Alpha, 2));

    zassert_equal(late_probe.Count(), 1U);
    zassert_equal(late_probe.values.front(), 2);
}

ZTEST(event_bus, test_a_throwing_subscriber_does_not_escape_Publish) {
    BusWithChannel fixture("eb_throwing");
    DispatchProbe probe;

    auto throwing_handle = fixture.channel.Subscribe(TestEventType::Alpha, [](const TestEvent&) {
        throw std::runtime_error("subscriber failed");
    });
    auto probe_handle = fixture.channel.Subscribe(TestEventType::Beta, [&probe](const TestEvent& event) { probe.Record(event); });
    zassert_true(throwing_handle.has_value());
    zassert_true(probe_handle.has_value());

    fixture.channel.Publish(MakeEvent(TestEventType::Alpha, 1));

    // The channel stays usable after a subscriber failure.
    fixture.channel.Publish(MakeEvent(TestEventType::Beta, 2));

    zassert_equal(probe.Count(), 1U);
    zassert_equal(probe.values.front(), 2);
}

ZTEST(event_bus, test_a_scoped_subscription_unsubscribes_when_it_is_destroyed) {
    BusWithChannel fixture("eb_scoped");
    DispatchProbe probe;

    {
        AnySubscription subscription = CreateScopedSubscription(
            fixture.channel,
            TestEventType::Alpha,
            [&probe](const TestEvent& event) { probe.Record(event); });
        zassert_not_null(subscription.get());

        fixture.channel.Publish(MakeEvent(TestEventType::Alpha, 1));

        zassert_equal(probe.Count(), 1U);
    }

    fixture.channel.Publish(MakeEvent(TestEventType::Alpha, 2));

    zassert_equal(probe.Count(), 1U);
}

ZTEST(event_bus, test_an_unregistered_channel_accepts_subscribers_but_swallows_publications) {
    TestChannel channel("eb_inert_ch");
    DispatchProbe probe;

    zassert_false(channel.IsRegistered());

    // Subscribing has to keep working, so a domain can hand out a channel the
    // composition root may never wire up.
    auto handle = channel.Subscribe(TestEventType::Alpha, [&probe](const TestEvent& event) { probe.Record(event); });
    zassert_true(handle.has_value());

    channel.Publish(MakeEvent(TestEventType::Alpha, 1));
    channel.PublishAsync(MakeEvent(TestEventType::Alpha, 2));

    zassert_equal(probe.Count(), 0U);
    zassert_false(channel.DrainOne(), "an unregistered channel must not queue anything");
}

ZTEST(event_bus, test_RegisterChannel_is_idempotent_for_the_same_bus) {
    BusWithChannel fixture("eb_register_twice");

    zassert_equal(fixture.bus.RegisterChannel(fixture.channel), 0);
    zassert_equal(fixture.bus.RegisterChannel(fixture.channel), 0);
    zassert_equal(fixture.channel.GetBus(), &fixture.bus);
}

ZTEST(event_bus, test_RegisterChannel_rejects_a_channel_owned_by_another_bus) {
    BusWithChannel fixture("eb_register_owned");
    TestEventBus second_bus("eb_register_second");

    zassert_equal(second_bus.RegisterChannel(fixture.channel), -EEXIST);
    zassert_equal(fixture.channel.GetBus(), &fixture.bus, "the original owner must be kept");
}

ZTEST(event_bus, test_destroying_a_bus_leaves_its_channels_inert) {
    TestChannel channel("eb_orphan_ch");

    {
        TestEventBus bus("eb_orphan");

        zassert_equal(bus.RegisterChannel(channel), 0);
        zassert_true(channel.IsRegistered());
    }

    zassert_false(channel.IsRegistered(), "~EventBus must detach its channels");

    // Would dereference the destroyed bus if the detach had not happened.
    channel.PublishAsync(MakeEvent(TestEventType::Alpha, 1));
}

ZTEST(event_bus, test_channels_sharing_a_bus_stay_isolated) {
    // Both channels outlive the bus, which is destroyed first and detaches them.
    TestChannel test_channel("eb_shared_test_ch");
    OtherChannel other_channel("eb_shared_other_ch");
    TestEventBus bus("eb_shared");

    zassert_equal(bus.RegisterChannel(test_channel), 0);
    zassert_equal(bus.RegisterChannel(other_channel), 0);

    DispatchProbe test_probe;
    int other_calls = 0;

    auto test_handle = test_channel.Subscribe(TestEventType::Alpha, [&test_probe](const TestEvent& event) { test_probe.Record(event); });
    auto other_handle = other_channel.Subscribe(OtherEventType::Signal, [&other_calls](const OtherChannel::EventMessage&) { ++other_calls; });
    zassert_true(test_handle.has_value());
    zassert_true(other_handle.has_value());

    test_channel.PublishAsync(MakeEvent(TestEventType::Alpha, 1));

    zassert_true(test_probe.WaitForDelivery(), "the event was never dispatched");
    zassert_equal(test_probe.Count(), 1U);
    zassert_equal(other_calls, 0, "an event must not leak into a peer channel");

    // The one bus drains both channels.
    other_channel.PublishAsync({ .type = OtherEventType::Signal, .payload = {}, .source_id = "test" });

    int64_t deadline = k_uptime_get() + DISPATCH_TIMEOUT_MS;
    while(other_calls == 0 && k_uptime_get() < deadline)
        k_msleep(1);

    zassert_equal(other_calls, 1);
}

ZTEST(event_bus, test_the_queue_drops_the_oldest_event_when_it_overflows) {
    constexpr size_t capacity = 4;
    constexpr int published = 7;

    TestChannel channel("eb_overflow_ch", capacity);
    ManualBus manual_bus;
    DispatchProbe probe;

    // Registering by hand keeps the events queued: nothing drains a ManualBus.
    channel.OnRegistered(&manual_bus);

    auto handle = channel.Subscribe(TestEventType::Alpha, [&probe](const TestEvent& event) { probe.Record(event); });
    zassert_true(handle.has_value());

    for(int i = 0; i < published; ++i)
        channel.PublishAsync(MakeEvent(TestEventType::Alpha, i));

    zassert_equal(manual_bus.wake_calls, published, "every publication must wake the bus");

    while(channel.DrainOne()) { }

    // Sheds the oldest rather than the newest, so the last `capacity` events survive.
    zassert_equal(probe.Count(), capacity);
    for(size_t i = 0; i < capacity; ++i)
        zassert_equal(probe.values[i], static_cast<int>(published - capacity + i));

    channel.OnRegistered(nullptr);
}

ZTEST(event_bus, test_DrainOne_reports_whether_it_dispatched) {
    TestChannel channel("eb_drain_ch");
    ManualBus manual_bus;

    channel.OnRegistered(&manual_bus);

    zassert_false(channel.DrainOne(), "an empty queue dispatches nothing");

    channel.PublishAsync(MakeEvent(TestEventType::Alpha, 1));

    zassert_true(channel.DrainOne());
    zassert_false(channel.DrainOne());

    channel.OnRegistered(nullptr);
}

ZTEST(event_bus, test_PublishAsync_delivers_the_event_on_the_bus_thread) {
    BusWithChannel fixture("eb_async");
    DispatchProbe probe;

    auto handle = fixture.channel.Subscribe(TestEventType::Alpha, [&probe](const TestEvent& event) { probe.Record(event); });
    zassert_true(handle.has_value());

    fixture.channel.PublishAsync(MakeEvent(TestEventType::Alpha, 99));

    zassert_true(probe.WaitForDelivery(), "the event was never dispatched");
    zassert_equal(probe.Count(), 1U);
    zassert_equal(probe.values.front(), 99);
    zassert_equal(probe.handler_thread_name, std::string("eb_async"));
}

ZTEST(event_bus, test_PublishAsync_preserves_the_publication_order) {
    BusWithChannel fixture("eb_async_order");
    DispatchProbe probe;

    auto handle = fixture.channel.Subscribe(TestEventType::Alpha, [&probe](const TestEvent& event) { probe.Record(event); });
    zassert_true(handle.has_value());

    constexpr int event_count = 5;
    for(int i = 0; i < event_count; ++i)
        fixture.channel.PublishAsync(MakeEvent(TestEventType::Alpha, i));

    for(int i = 0; i < event_count; ++i)
        zassert_true(probe.WaitForDelivery(), "event %d was never dispatched", i);

    zassert_equal(probe.Count(), static_cast<size_t>(event_count));
    for(int i = 0; i < event_count; ++i)
        zassert_equal(probe.values[i], i);
}

ZTEST(event_bus, test_PublishAsync_applies_the_filters) {
    BusWithChannel fixture("eb_async_filtered");
    DispatchProbe probe;
    DispatchProbe accepted;

    auto filtered_handle = fixture.channel.Subscribe(
        TestEventType::Alpha,
        SourceFilter{"sensor_1"},
        [&probe](const TestEvent& event) { probe.Record(event); });
    auto accepted_handle = fixture.channel.Subscribe(TestEventType::Alpha, [&accepted](const TestEvent& event) { accepted.Record(event); });
    zassert_true(filtered_handle.has_value());
    zassert_true(accepted_handle.has_value());

    fixture.channel.PublishAsync(MakeEvent(TestEventType::Alpha, 1, "sensor_2"));

    zassert_true(accepted.WaitForDelivery(), "the event was never dispatched");
    zassert_equal(probe.Count(), 0U);
}

ZTEST(event_bus, test_PublishAsync_stops_delivering_to_an_unsubscribed_handler) {
    BusWithChannel fixture("eb_async_unsubscribed");
    DispatchProbe removed;
    DispatchProbe kept;

    auto removed_handle = fixture.channel.Subscribe(TestEventType::Alpha, [&removed](const TestEvent& event) { removed.Record(event); });
    auto kept_handle = fixture.channel.Subscribe(TestEventType::Alpha, [&kept](const TestEvent& event) { kept.Record(event); });
    zassert_true(removed_handle.has_value());
    zassert_true(kept_handle.has_value());

    zassert_true(fixture.channel.Unsubscribe(removed_handle.value()));

    fixture.channel.PublishAsync(MakeEvent(TestEventType::Alpha, 1));

    zassert_true(kept.WaitForDelivery(), "the event was never dispatched");
    zassert_equal(removed.Count(), 0U);
}

ZTEST(event_bus, test_a_throwing_subscriber_does_not_stop_the_async_dispatch_loop) {
    BusWithChannel fixture("eb_async_throwing");
    DispatchProbe probe;

    auto throwing_handle = fixture.channel.Subscribe(TestEventType::Alpha, [](const TestEvent&) {
        throw std::runtime_error("subscriber failed");
    });
    auto probe_handle = fixture.channel.Subscribe(TestEventType::Beta, [&probe](const TestEvent& event) { probe.Record(event); });
    zassert_true(throwing_handle.has_value());
    zassert_true(probe_handle.has_value());

    fixture.channel.PublishAsync(MakeEvent(TestEventType::Alpha, 1));
    fixture.channel.PublishAsync(MakeEvent(TestEventType::Beta, 2));

    zassert_true(probe.WaitForDelivery(), "the queue stalled on the failing subscriber");
    zassert_equal(probe.values.front(), 2);
}
