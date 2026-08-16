#include <cstddef>
#include <type_traits>
#include <utility>

#include <zephyr/ztest.h>

#include "subsys/event_bus/subscription_handle.h"

#include "test_events.h"

using eerie_leap::subsys::event_bus::SubscriptionHandle;

using event_bus_tests::TestEventType;

namespace {

using TestSubscriptionHandle = SubscriptionHandle<TestEventType>;

} // namespace

ZTEST_SUITE(event_bus_subscription_handle, NULL, NULL, NULL, NULL, NULL);

ZTEST(event_bus_subscription_handle, test_a_new_handle_is_valid_and_keeps_its_id_and_event_type) {
    TestSubscriptionHandle handle(3, TestEventType::Beta);

    zassert_equal(handle.GetId(), static_cast<size_t>(3));
    zassert_equal(handle.GetEventType(), TestEventType::Beta);
    zassert_true(handle.IsValid());
}

ZTEST(event_bus_subscription_handle, test_Invalidate_marks_the_handle_unusable_without_losing_its_identity) {
    TestSubscriptionHandle handle(3, TestEventType::Beta);

    handle.Invalidate();

    zassert_false(handle.IsValid());
    zassert_equal(handle.GetId(), static_cast<size_t>(3));
    zassert_equal(handle.GetEventType(), TestEventType::Beta);
}

ZTEST(event_bus_subscription_handle, test_Invalidate_can_be_repeated) {
    TestSubscriptionHandle handle(3, TestEventType::Beta);

    handle.Invalidate();
    handle.Invalidate();

    zassert_false(handle.IsValid());
}

ZTEST(event_bus_subscription_handle, test_moving_a_handle_transfers_the_validity_to_the_destination) {
    TestSubscriptionHandle source(5, TestEventType::Gamma);
    TestSubscriptionHandle moved(std::move(source));

    zassert_true(moved.IsValid());
    zassert_equal(moved.GetId(), static_cast<size_t>(5));
    zassert_equal(moved.GetEventType(), TestEventType::Gamma);
    zassert_false(source.IsValid()); // NOLINT(bugprone-use-after-move) - the moved-from handle must be released
}

ZTEST(event_bus_subscription_handle, test_moving_an_invalidated_handle_keeps_the_destination_invalid) {
    TestSubscriptionHandle source(5, TestEventType::Gamma);
    source.Invalidate();

    TestSubscriptionHandle moved(std::move(source));

    zassert_false(moved.IsValid());
    zassert_equal(moved.GetId(), static_cast<size_t>(5));
}

static_assert(!std::is_copy_constructible_v<TestSubscriptionHandle>);
static_assert(!std::is_copy_assignable_v<TestSubscriptionHandle>);
static_assert(std::is_move_constructible_v<TestSubscriptionHandle>);
