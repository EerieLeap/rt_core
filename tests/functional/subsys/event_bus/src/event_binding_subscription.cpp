#include <array>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "utilities/string/string_helpers.h"

#include "subsys/event_bus/event_binding.h"
#include "subsys/event_bus/event_binding_subscription.h"
#include "subsys/event_bus/event_channel_id.h"

#include "binding_test_channels.h"

using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::event_bus::EventBinding;
using eerie_leap::subsys::event_bus::EventBindingHandler;
using eerie_leap::subsys::event_bus::EventChannelId;
using eerie_leap::subsys::event_bus::EventData;
using eerie_leap::subsys::event_bus::GetEventBindingError;
using eerie_leap::subsys::event_bus::SubscribeEventBinding;
using eerie_leap::utilities::string::StringHelpers;
using eerie_leap::utilities::type::ConfigValue;

using event_binding_tests::BINDING_TEST_SOURCE;
using event_binding_tests::BindingEventType;
using event_binding_tests::BindingPayloadType;
using event_binding_tests::FakeView;
using event_binding_tests::PublishSync;
using event_binding_tests::RegisterBindingTestChannels;
using event_binding_tests::ResetBindingTestChannels;
using event_binding_tests::SetUpBindingTestChannels;

namespace {

constexpr int K_DISPATCH_TIMEOUT_MS = 1000;

std::string CurrentThreadName() {
    const char* name = k_thread_name_get(k_current_get());

    return name != nullptr ? std::string(name) : std::string();
}

// The handler shares the probe's state instead of pointing at the probe. Three suites share the
// channel, and a failed assertion aborts the test thread without unwinding, so a subscription it
// leaves behind must not reach into a dead stack frame.
class HandlerProbe {
private:
    struct State {
        int calls = 0;
        std::optional<EventData> last_value;
        std::string thread_name;
        k_sem delivered{};
    };

    std::shared_ptr<State> state_ = std::make_shared<State>();

public:
    HandlerProbe() {
        k_sem_init(&state_->delivered, 0, K_SEM_MAX_LIMIT);
    }

    EventBindingHandler Handler() const {
        return [state = state_](const EventData& value) {
            ++state->calls;
            state->last_value = value;
            state->thread_name = CurrentThreadName();

            k_sem_give(&state->delivered);
        };
    }

    int Calls() const {
        return state_->calls;
    }
    const std::optional<EventData>& LastValue() const {
        return state_->last_value;
    }
    const std::string& ThreadName() const {
        return state_->thread_name;
    }
    bool WaitForDelivery(k_timeout_t timeout) const {
        return k_sem_take(&state_->delivered, timeout) == 0;
    }
};

// Updated/Value on the channel registered as Sensors, unless a test says otherwise.
EventBinding ValueBinding() {
    return EventBinding{ .channel = EventChannelId::Sensors,
        .event_type = std::to_underlying(BindingEventType::Updated),
        .payload_key = std::to_underlying(BindingPayloadType::Value) };
}

EventBinding SelectedBinding(BindingPayloadType selector_key, ConfigValue selector_value) {
    auto binding = ValueBinding();
    binding.selector_key = std::to_underlying(selector_key);
    binding.selector_value = std::move(selector_value);

    return binding;
}

void PublishValue(BindingPayloadType selector_key, EventData selector, float value = 1.0F) {
    PublishSync(
        RegisterBindingTestChannels().primary, BindingEventType::Updated,
        { { selector_key, std::move(selector) }, { BindingPayloadType::Value, value } }
    );
}

} // namespace

ZTEST_SUITE(event_binding_subscription, NULL, SetUpBindingTestChannels, ResetBindingTestChannels, NULL, NULL);

ZTEST(event_binding_subscription, test_a_binding_to_None_does_not_subscribe) {
    const HandlerProbe probe;
    auto binding = ValueBinding();
    binding.channel = EventChannelId::None;

    zassert_is_null(SubscribeEventBinding(binding, probe.Handler()).get());
}

ZTEST(event_binding_subscription, test_a_binding_to_an_unregistered_channel_does_not_subscribe) {
    const HandlerProbe probe;
    auto binding = ValueBinding();

    binding.channel = EventChannelId::Navigation;
    zassert_is_null(SubscribeEventBinding(binding, probe.Handler()).get());

    binding.channel = static_cast<EventChannelId>(99);
    zassert_is_null(SubscribeEventBinding(binding, probe.Handler()).get());
}

// Unfiltered, it would take every sensor's value.
ZTEST(event_binding_subscription, test_a_selector_that_cannot_select_does_not_subscribe) {
    const HandlerProbe probe;
    const auto subscription = SubscribeEventBinding(SelectedBinding(BindingPayloadType::Id, 1.5), probe.Handler());

    zassert_is_null(subscription.get());

    PublishValue(BindingPayloadType::Id, uint32_t{ 1 });

    zassert_equal(probe.Calls(), 0);
}

ZTEST(event_binding_subscription, test_an_empty_handler_does_not_subscribe) {
    zassert_is_null(SubscribeEventBinding(ValueBinding(), EventBindingHandler{}).get());
}

ZTEST(event_binding_subscription, test_a_refused_subscription_is_null) {
    auto& channels = RegisterBindingTestChannels();
    const HandlerProbe probe;

    channels.primary.refuse_next = true;
    const auto subscription = SubscribeEventBinding(ValueBinding(), probe.Handler());

    zassert_is_null(subscription.get());
    zassert_false(channels.primary.refuse_next, "The null did not come from the channel's refusal.");

    PublishValue(BindingPayloadType::Id, uint32_t{ 1 });

    zassert_equal(probe.Calls(), 0);
}

ZTEST(event_binding_subscription, test_the_bound_payload_value_reaches_the_handler) {
    const HandlerProbe probe;
    const auto subscription = SubscribeEventBinding(ValueBinding(), probe.Handler());
    zassert_not_null(subscription.get());

    PublishValue(BindingPayloadType::Id, uint32_t{ 1 }, 42.5F);

    zassert_equal(probe.Calls(), 1);
    zassert_true(probe.LastValue().has_value());
    zassert_equal(std::get<float>(*probe.LastValue()), 42.5F);
}

ZTEST(event_binding_subscription, test_an_event_without_the_payload_key_does_not_reach_the_handler) {
    const HandlerProbe probe;
    const auto subscription = SubscribeEventBinding(ValueBinding(), probe.Handler());
    zassert_not_null(subscription.get());

    PublishSync(
        RegisterBindingTestChannels().primary, BindingEventType::Updated, { { BindingPayloadType::Id, uint32_t{ 1 } } }
    );

    zassert_equal(probe.Calls(), 0);
}

ZTEST(event_binding_subscription, test_the_binding_honours_the_event_type) {
    auto& channels = RegisterBindingTestChannels();
    const HandlerProbe probe;
    const auto subscription = SubscribeEventBinding(ValueBinding(), probe.Handler());
    zassert_not_null(subscription.get());

    PublishSync(channels.primary, BindingEventType::Other, { { BindingPayloadType::Value, 1.0F } });
    zassert_equal(probe.Calls(), 0);

    PublishSync(channels.primary, BindingEventType::Updated, { { BindingPayloadType::Value, 1.0F } });
    zassert_equal(probe.Calls(), 1);
}

ZTEST(event_binding_subscription, test_a_text_selector_matches_the_hashed_id) {
    const HandlerProbe probe;
    const auto subscription = SubscribeEventBinding(
        SelectedBinding(BindingPayloadType::Id, std::pmr::string("left")), probe.Handler()
    );
    zassert_not_null(subscription.get());

    PublishValue(BindingPayloadType::Id, StringHelpers::GetHash("right"));
    zassert_equal(probe.Calls(), 0);

    PublishValue(BindingPayloadType::Id, StringHelpers::GetHash("left"));
    zassert_equal(probe.Calls(), 1);
}

ZTEST(event_binding_subscription, test_an_integer_selector_matches_an_int_or_uint32_id) {
    const HandlerProbe probe;
    const auto subscription = SubscribeEventBinding(SelectedBinding(BindingPayloadType::Id, 7), probe.Handler());
    zassert_not_null(subscription.get());

    PublishValue(BindingPayloadType::Id, 7);
    PublishValue(BindingPayloadType::Id, uint32_t{ 7 });
    PublishValue(BindingPayloadType::Id, 8);
    PublishValue(BindingPayloadType::Id, uint32_t{ 8 });

    zassert_equal(probe.Calls(), 2);
}

ZTEST(event_binding_subscription, test_a_bool_selector_matches_a_bool_entry) {
    const HandlerProbe probe;
    const auto subscription = SubscribeEventBinding(SelectedBinding(BindingPayloadType::Flag, true), probe.Handler());
    zassert_not_null(subscription.get());

    PublishValue(BindingPayloadType::Flag, false);
    zassert_equal(probe.Calls(), 0);

    PublishValue(BindingPayloadType::Flag, true);
    zassert_equal(probe.Calls(), 1);
}

// What puts the selector under the subscriber lock: the channel receives it as its filter.
ZTEST(event_binding_subscription, test_the_selector_is_the_channel_filter) {
    const auto& channels = RegisterBindingTestChannels();
    const HandlerProbe probe;

    const auto selected = SubscribeEventBinding(
        SelectedBinding(BindingPayloadType::Id, std::pmr::string("left")), probe.Handler()
    );
    zassert_not_null(selected.get());
    zassert_true(channels.primary.last_had_filter);

    const std::array<std::pair<uint32_t, EventData>, 1> left{ std::pair{
        std::to_underlying(BindingPayloadType::Id), EventData{ StringHelpers::GetHash("left") } } };
    const std::array<std::pair<uint32_t, EventData>, 1> right{ std::pair{
        std::to_underlying(BindingPayloadType::Id), EventData{ StringHelpers::GetHash("right") } } };
    const std::array<std::pair<uint32_t, EventData>, 1> without_id{ std::pair{
        std::to_underlying(BindingPayloadType::Value), EventData{ 1.0F } } };

    zassert_true(channels.primary.last_filter(FakeView(left)));
    zassert_false(channels.primary.last_filter(FakeView(right)));
    zassert_false(channels.primary.last_filter(FakeView(without_id)));

    const auto unconditional = SubscribeEventBinding(ValueBinding(), probe.Handler());
    zassert_not_null(unconditional.get());
    zassert_false(channels.primary.last_had_filter);
}

ZTEST(event_binding_subscription, test_destroying_the_subscription_unsubscribes) {
    const HandlerProbe removed;
    const HandlerProbe observer;

    auto subscription = SubscribeEventBinding(ValueBinding(), removed.Handler());
    zassert_not_null(subscription.get());

    PublishValue(BindingPayloadType::Id, uint32_t{ 1 });
    zassert_equal(removed.Calls(), 1);

    subscription.reset();

    const auto observing = SubscribeEventBinding(ValueBinding(), observer.Handler());
    zassert_not_null(observing.get());

    PublishValue(BindingPayloadType::Id, uint32_t{ 1 });

    zassert_equal(removed.Calls(), 1);
    zassert_equal(observer.Calls(), 1);
}

ZTEST(event_binding_subscription, test_an_async_publish_reaches_the_handler_on_the_bus_thread) {
    auto& channels = RegisterBindingTestChannels();
    const HandlerProbe probe;
    const auto subscription = SubscribeEventBinding(ValueBinding(), probe.Handler());
    zassert_not_null(subscription.get());

    channels.primary.PublishAsync(
        { .source_id = BINDING_TEST_SOURCE,
            .type = BindingEventType::Updated,
            .payload = { { BindingPayloadType::Value, 3.0F } } }
    );

    zassert_true(probe.WaitForDelivery(K_MSEC(K_DISPATCH_TIMEOUT_MS)));
    zassert_equal(probe.Calls(), 1);
    zassert_equal(std::get<float>(*probe.LastValue()), 3.0F);
    zassert_equal(probe.ThreadName(), std::string("binding_test_bus"));
}

ZTEST(event_binding_subscription, test_ChannelsKnowTheirLastEventTypeAndPayloadKey) {
    const auto& channel = RegisterBindingTestChannels().primary;

    zassert_true(channel.HasEventType(std::to_underlying(BindingEventType::Updated)));
    zassert_true(channel.HasEventType(std::to_underlying(BindingEventType::Other)), "The last value counts");
    zassert_false(channel.HasEventType(std::to_underlying(BindingEventType::Other) + 1));
    zassert_false(channel.HasEventType(0xFFFFFFFF));

    zassert_true(channel.HasPayloadKey(std::to_underlying(BindingPayloadType::Id)));
    zassert_true(channel.HasPayloadKey(std::to_underlying(BindingPayloadType::Flag)), "The last value counts");
    zassert_false(channel.HasPayloadKey(std::to_underlying(BindingPayloadType::Flag) + 1));
    zassert_false(channel.HasPayloadKey(0xFFFFFFFF));
}

ZTEST(event_binding_subscription, test_AnErrorIsGivenForABindingThisUnitCannotResolve) {
    zassert_true(GetEventBindingError(ValueBinding()).empty(), "A resolvable binding has none");
    zassert_true(
        GetEventBindingError(SelectedBinding(BindingPayloadType::Id, std::pmr::string("a"))).empty(),
        "nor does one with a selector"
    );

    auto unregistered = ValueBinding();
    unregistered.channel = EventChannelId::Navigation;
    zassert_false(GetEventBindingError(unregistered).empty(), "A channel nobody registered");

    auto none = ValueBinding();
    none.channel = EventChannelId::None;
    zassert_false(GetEventBindingError(none).empty(), "None");

    auto event_type = ValueBinding();
    event_type.event_type = std::to_underlying(BindingEventType::Other) + 1;
    const auto event_error = GetEventBindingError(event_type);
    zassert_true(event_error.contains("event type"), "%.*s", static_cast<int>(event_error.size()), event_error.data());

    auto payload_key = ValueBinding();
    payload_key.payload_key = std::to_underlying(BindingPayloadType::Flag) + 1;
    const auto key_error = GetEventBindingError(payload_key);
    zassert_true(key_error.contains("payload key"), "%.*s", static_cast<int>(key_error.size()), key_error.data());

    auto selector = SelectedBinding(BindingPayloadType::Id, std::pmr::string("a"));
    selector.selector_key = std::to_underlying(BindingPayloadType::Flag) + 1;
    const auto selector_error = GetEventBindingError(selector);
    zassert_true(
        selector_error.contains("selects"), "%.*s", static_cast<int>(selector_error.size()), selector_error.data()
    );

    // Without a selector its key is ignored, as the subscription ignores it.
    auto no_selector = ValueBinding();
    no_selector.selector_key = 0xFFFFFFFF;
    zassert_true(GetEventBindingError(no_selector).empty());
}
