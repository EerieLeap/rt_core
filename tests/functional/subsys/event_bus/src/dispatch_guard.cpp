#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "subsys/event_bus/dispatch_guard.h"
#include "subsys/event_bus/event_binding.h"
#include "subsys/event_bus/event_binding_subscription.h"
#include "subsys/event_bus/event_channel_id.h"

#include "binding_test_channels.h"

using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::event_bus::DispatchGuard;
using eerie_leap::subsys::event_bus::EventBinding;
using eerie_leap::subsys::event_bus::EventChannelId;
using eerie_leap::subsys::event_bus::EventData;
using eerie_leap::subsys::event_bus::SubscribeEventBinding;

using event_binding_tests::BindingEventType;
using event_binding_tests::BindingPayloadType;
using event_binding_tests::PublishSync;
using event_binding_tests::RegisterBindingTestChannels;
using event_binding_tests::ResetBindingTestChannels;
using event_binding_tests::SetUpBindingTestChannels;

namespace {

constexpr int k_helper_priority = 5;

K_THREAD_STACK_DEFINE(dispatch_helper_stack, 4096);
k_thread dispatch_helper;

// Owned jointly by the test and the helper thread, so a failed assertion, which aborts the test
// thread without unwinding, never leaves the helper writing to a dead stack frame.
struct DetachRace {
    int owner = 0;
    DispatchGuard guard{ &owner };
    k_sem entered;
    std::atomic<bool> finished{ false };

    DetachRace() {
        k_sem_init(&entered, 0, 1);
    }
};

void DispatchSlowly(void* race_holder, void*, void*) {
    const std::unique_ptr<std::shared_ptr<DetachRace>> holder(static_cast<std::shared_ptr<DetachRace>*>(race_holder));
    auto& race = **holder;

    race.guard.Dispatch([&race] {
        k_sem_give(&race.entered);
        k_msleep(20);
        race.finished.store(true);
    });
}

// Shared by both handlers instead of captured by reference, so a failed assertion, which aborts the
// test thread without unwinding, leaves no subscription on the shared channel pointing into a dead
// stack frame.
struct RemovalRace {
    int owner_b = 0;
    int a_calls = 0;
    int b_handler_calls = 0;
    int b_body_calls = 0;
    AnySubscription subscription_b;
};

EventBinding ValueBinding() {
    return EventBinding{ .channel = EventChannelId::Sensors,
        .event_type = std::to_underlying(BindingEventType::Updated),
        .payload_key = std::to_underlying(BindingPayloadType::Value) };
}

} // namespace

ZTEST_SUITE(dispatch_guard, NULL, SetUpBindingTestChannels, ResetBindingTestChannels, NULL, NULL);

ZTEST(dispatch_guard, test_Dispatch_runs_the_body_while_attached) {
    int owner = 0;
    DispatchGuard guard(&owner);
    int runs = 0;

    guard.Dispatch([&runs] {
        ++runs;
    });

    zassert_equal(runs, 1);
}

ZTEST(dispatch_guard, test_Dispatch_is_a_no_op_after_Detach) {
    int owner = 0;
    DispatchGuard guard(&owner);
    int runs = 0;

    guard.Detach();
    guard.Dispatch([&runs] {
        ++runs;
    });

    zassert_equal(runs, 0);
}

ZTEST(dispatch_guard, test_Detach_can_be_repeated) {
    int owner = 0;
    DispatchGuard guard(&owner);
    int runs = 0;

    guard.Detach();
    guard.Detach();
    guard.Dispatch([&runs] {
        ++runs;
    });

    zassert_equal(runs, 0);
}

// The helper holds the guard's lock from before it signals until its body returns, so Detach()
// can only return after the body, whatever the scheduler does in between.
ZTEST(dispatch_guard, test_Detach_waits_for_an_in_flight_dispatch) {
    const auto race = std::make_shared<DetachRace>();

    k_thread_create(
        &dispatch_helper, dispatch_helper_stack, K_THREAD_STACK_SIZEOF(dispatch_helper_stack), DispatchSlowly,
        new std::shared_ptr<DetachRace>(race), nullptr, nullptr, k_helper_priority, 0, K_NO_WAIT
    );

    zassert_ok(k_sem_take(&race->entered, K_SECONDS(1)));

    race->guard.Detach();

    // Taken before the join and asserted after it, so a failure leaves no thread behind.
    const bool finished_at_detach = race->finished.load();

    zassert_ok(k_thread_join(&dispatch_helper, K_SECONDS(1)));
    zassert_true(finished_at_detach, "Detach() returned while a dispatch was still running.");

    int runs = 0;
    race->guard.Dispatch([&runs] {
        ++runs;
    });
    zassert_equal(runs, 0);
}

// The race the guard exists for: a dispatch already snapshotted still calls a handler whose owner
// has gone, and only the guard turns that call into a no-op.
ZTEST(dispatch_guard, test_a_guarded_handler_removed_during_dispatch_does_nothing) {
    auto& channels = RegisterBindingTestChannels();

    auto race = std::make_shared<RemovalRace>();
    const auto guard_b = std::make_shared<DispatchGuard>(&race->owner_b);

    const auto subscription_a = SubscribeEventBinding(ValueBinding(), [race, guard_b](const EventData&) {
        ++race->a_calls;

        // B's owner tears down the way a widget does: detach first, then drop the subscription.
        guard_b->Detach();
        race->subscription_b.reset();
    });
    zassert_not_null(subscription_a.get());

    race->subscription_b = SubscribeEventBinding(ValueBinding(), [race, guard = guard_b](const EventData&) {
        ++race->b_handler_calls;

        guard->Dispatch([&race] {
            ++race->b_body_calls;
        });
    });
    zassert_not_null(race->subscription_b.get());

    PublishSync(channels.primary, BindingEventType::Updated, { { BindingPayloadType::Value, 1.0F } });

    zassert_equal(race->a_calls, 1);
    zassert_equal(race->b_handler_calls, 1, "The snapshot no longer delivers to a subscriber removed during dispatch.");
    zassert_equal(race->b_body_calls, 0);
    zassert_is_null(race->subscription_b.get());

    PublishSync(channels.primary, BindingEventType::Updated, { { BindingPayloadType::Value, 2.0F } });

    zassert_equal(race->a_calls, 2);
    zassert_equal(race->b_handler_calls, 1);
    zassert_equal(race->b_body_calls, 0);
}
