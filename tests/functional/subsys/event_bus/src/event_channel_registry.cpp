#include <cerrno>

#include <zephyr/ztest.h>

#include "subsys/event_bus/event_channel_id.h"
#include "subsys/event_bus/event_channel_registry.h"

#include "binding_test_channels.h"

using eerie_leap::subsys::event_bus::EventChannelId;
using eerie_leap::subsys::event_bus::EventChannelRegistry;

using event_binding_tests::RegisterBindingTestChannels;
using event_binding_tests::ResetBindingTestChannels;
using event_binding_tests::SetUpBindingTestChannels;

// The registry is one per program, so no case may leave a registration another relies on: refused
// registrations change nothing by contract, and the only accepted one repeats an existing one.
ZTEST_SUITE(event_channel_registry, NULL, SetUpBindingTestChannels, ResetBindingTestChannels, NULL, NULL);

ZTEST(event_channel_registry, test_a_registered_channel_resolves) {
    auto& channels = RegisterBindingTestChannels();
    const auto& registry = EventChannelRegistry::GetInstance();

    zassert_equal(registry.Find(EventChannelId::Sensors), &channels.primary);
    zassert_equal(registry.Find(EventChannelId::Logging), &channels.secondary);
}

// A binding stored before its channel existed, or for another unit's channel, must be inert.
ZTEST(event_channel_registry, test_None_and_unregistered_or_unknown_ids_resolve_to_null) {
    const auto& registry = EventChannelRegistry::GetInstance();

    zassert_is_null(registry.Find(EventChannelId::None));
    zassert_is_null(registry.Find(EventChannelId::Navigation));
    zassert_is_null(registry.Find(static_cast<EventChannelId>(7)));
    zassert_is_null(registry.Find(static_cast<EventChannelId>(99)));
}

ZTEST(event_channel_registry, test_registering_the_same_channel_again_succeeds) {
    auto& channels = RegisterBindingTestChannels();
    auto& registry = EventChannelRegistry::GetInstance();

    zassert_ok(registry.Register(EventChannelId::Sensors, channels.primary));
    zassert_equal(registry.Find(EventChannelId::Sensors), &channels.primary);
}

ZTEST(event_channel_registry, test_a_taken_id_refuses_another_channel) {
    auto& channels = RegisterBindingTestChannels();
    auto& registry = EventChannelRegistry::GetInstance();

    zassert_equal(registry.Register(EventChannelId::Sensors, channels.secondary), -EEXIST);
    zassert_equal(registry.Find(EventChannelId::Sensors), &channels.primary);
}

// Only ids IsValidEventChannelId() names can be registered, not merely ids that fit the table.
ZTEST(event_channel_registry, test_None_and_unknown_ids_are_refused) {
    auto& channels = RegisterBindingTestChannels();
    auto& registry = EventChannelRegistry::GetInstance();

    zassert_equal(registry.Register(EventChannelId::None, channels.primary), -EINVAL);
    zassert_equal(registry.Register(static_cast<EventChannelId>(5), channels.primary), -EINVAL);
    zassert_equal(registry.Register(static_cast<EventChannelId>(8), channels.primary), -EINVAL);
    zassert_equal(registry.Register(static_cast<EventChannelId>(99), channels.primary), -EINVAL);

    zassert_is_null(registry.Find(EventChannelId::None));
    zassert_is_null(registry.Find(static_cast<EventChannelId>(5)));
}
