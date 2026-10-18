#include <cstdint>
#include <memory_resource>
#include <string>
#include <utility>
#include <variant>

#include <zephyr/ztest.h>

#include "utilities/string/string_helpers.h"
#include "utilities/type/config_value.h"

#include "subsys/event_bus/event.h"
#include "subsys/event_bus/event_binding.h"
#include "subsys/event_bus/event_binding_subscription.h"
#include "subsys/event_bus/event_channel_id.h"

using eerie_leap::subsys::event_bus::EventBinding;
using eerie_leap::subsys::event_bus::EventChannelId;
using eerie_leap::subsys::event_bus::EventData;
using eerie_leap::subsys::event_bus::IsSelectorValue;
using eerie_leap::subsys::event_bus::IsValidEventChannelId;
using eerie_leap::subsys::event_bus::LAST_EVENT_CHANNEL_ID;
using eerie_leap::subsys::event_bus::SelectorMatches;
using eerie_leap::subsys::event_bus::ToSelectorValue;
using eerie_leap::utilities::string::StringHelpers;
using eerie_leap::utilities::type::ConfigValue;

namespace {

// Every alternative a selector must not take.
ConfigValue NonSelectors(size_t index) {
    switch(index) {
        case 0:
            return ConfigValue{};
        case 1:
            return ConfigValue{ 1.5 };
        case 2:
            return ConfigValue{ std::pmr::vector<int>{ 1, 2 } };
        case 3:
            return ConfigValue{ std::pmr::vector<std::pmr::string>{ "a" } };
        default:
            return ConfigValue{ std::pmr::unordered_map<std::pmr::string, std::pmr::string>{ { "a", "b" } } };
    }
}

constexpr size_t NON_SELECTOR_COUNT = 5;

} // namespace

ZTEST_SUITE(event_bus_event_binding, NULL, NULL, NULL, NULL, NULL);

// Stored configurations carry these numbers; renumbering would rebind every stored binding.
ZTEST(event_bus_event_binding, test_EventChannelId_keeps_its_persisted_values) {
    zassert_equal(std::to_underlying(EventChannelId::None), 0U);
    zassert_equal(std::to_underlying(EventChannelId::Sensors), 1U);
    zassert_equal(std::to_underlying(EventChannelId::Logging), 2U);
    zassert_equal(std::to_underlying(EventChannelId::Navigation), 3U);
    zassert_equal(std::to_underlying(EventChannelId::Settings), 4U);
}

ZTEST(event_bus_event_binding, test_IsValidEventChannelId_accepts_only_named_channels) {
    zassert_false(IsValidEventChannelId(EventChannelId::None));
    zassert_false(IsValidEventChannelId(static_cast<EventChannelId>(5)));
    zassert_false(IsValidEventChannelId(static_cast<EventChannelId>(99)));

    zassert_true(IsValidEventChannelId(EventChannelId::Sensors));
    zassert_true(IsValidEventChannelId(EventChannelId::Logging));
    zassert_true(IsValidEventChannelId(EventChannelId::Navigation));
    zassert_true(IsValidEventChannelId(EventChannelId::Settings));
}

// The registry sizes its table from the last id, so the bound must stop exactly there.
ZTEST(event_bus_event_binding, test_IsValidEventChannelId_stops_at_the_last_id) {
    const auto last = std::to_underlying(LAST_EVENT_CHANNEL_ID);

    zassert_true(IsValidEventChannelId(LAST_EVENT_CHANNEL_ID));
    zassert_false(IsValidEventChannelId(static_cast<EventChannelId>(last + 1)));
}

ZTEST(event_bus_event_binding, test_a_binding_has_a_selector_only_with_a_value) {
    EventBinding binding;
    zassert_false(binding.HasSelector());

    // 0 is a live payload key, so the key alone cannot mean "selected".
    binding.selector_key = 3;
    zassert_false(binding.HasSelector());

    binding.selector_value = 0;
    zassert_true(binding.HasSelector());
}

ZTEST(event_bus_event_binding, test_IsSelectorValue_accepts_text_integers_and_bools) {
    zassert_true(IsSelectorValue(ConfigValue{ std::pmr::string("sensor_1") }));
    zassert_true(IsSelectorValue(ConfigValue{ 7 }));
    zassert_true(IsSelectorValue(ConfigValue{ true }));

    for(size_t i = 0; i < NON_SELECTOR_COUNT; ++i)
        zassert_false(IsSelectorValue(NonSelectors(i)), "Alternative %u accepted.", static_cast<unsigned>(i));
}

ZTEST(event_bus_event_binding, test_ToSelectorValue_hashes_text_as_payloads_carry_ids) {
    auto selector = ToSelectorValue(ConfigValue{ std::pmr::string("sensor_1") });

    zassert_true(selector.has_value());
    zassert_true(std::holds_alternative<uint32_t>(*selector));
    zassert_equal(std::get<uint32_t>(*selector), StringHelpers::GetHash("sensor_1"));
}

ZTEST(event_bus_event_binding, test_ToSelectorValue_keeps_integers_and_bools) {
    auto number = ToSelectorValue(ConfigValue{ 7 });
    zassert_true(number.has_value());
    zassert_true(std::holds_alternative<int>(*number));
    zassert_equal(std::get<int>(*number), 7);

    auto flag = ToSelectorValue(ConfigValue{ true });
    zassert_true(flag.has_value());
    zassert_true(std::holds_alternative<bool>(*flag));
    zassert_true(std::get<bool>(*flag));
}

ZTEST(event_bus_event_binding, test_ToSelectorValue_rejects_what_cannot_select) {
    for(size_t i = 0; i < NON_SELECTOR_COUNT; ++i)
        zassert_false(
            ToSelectorValue(NonSelectors(i)).has_value(), "Alternative %u resolved.", static_cast<unsigned>(i)
        );
}

ZTEST(event_bus_event_binding, test_a_hash_selector_matches_only_the_same_uint32) {
    const uint32_t hash = StringHelpers::GetHash("sensor_1");
    const EventData selector{ hash };

    zassert_true(SelectorMatches(selector, EventData{ hash }));
    zassert_false(SelectorMatches(selector, EventData{ hash + 1 }));
    zassert_false(SelectorMatches(selector, EventData{ static_cast<int>(hash) }));
    zassert_false(SelectorMatches(selector, EventData{ std::string("sensor_1") }));
}

ZTEST(event_bus_event_binding, test_an_integer_selector_matches_the_same_int_or_uint32) {
    const EventData selector{ 7 };

    zassert_true(SelectorMatches(selector, EventData{ 7 }));
    zassert_true(SelectorMatches(selector, EventData{ uint32_t{ 7 } }));

    zassert_false(SelectorMatches(selector, EventData{ 8 }));
    zassert_false(SelectorMatches(selector, EventData{ uint32_t{ 8 } }));
    zassert_false(SelectorMatches(selector, EventData{ 7.0F }));
    zassert_false(SelectorMatches(selector, EventData{ true }));
}

ZTEST(event_bus_event_binding, test_a_bool_selector_matches_only_the_same_bool) {
    const EventData selector{ true };

    zassert_true(SelectorMatches(selector, EventData{ true }));

    zassert_false(SelectorMatches(selector, EventData{ false }));
    zassert_false(SelectorMatches(selector, EventData{ 1 }));
    zassert_false(SelectorMatches(selector, EventData{ uint32_t{ 1 } }));
}
