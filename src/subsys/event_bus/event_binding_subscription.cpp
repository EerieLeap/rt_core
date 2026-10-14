#include <cstdint>
#include <memory_resource>
#include <string>
#include <utility>
#include <variant>

#include "utilities/string/string_helpers.h"

#include "erased_payload_view.h"
#include "event_channel_registry.h"
#include "event_binding_subscription.h"

namespace eerie_leap::subsys::event_bus {

using eerie_leap::utilities::string::StringHelpers;

std::optional<EventData> ToSelectorValue(const ConfigValue& value) {
    if(const auto* text = std::get_if<std::pmr::string>(&value))
        return EventData { StringHelpers::GetHash(*text) };

    if(const auto* number = std::get_if<int>(&value))
        return EventData { *number };

    if(const auto* flag = std::get_if<bool>(&value))
        return EventData { *flag };

    return std::nullopt;
}

bool SelectorMatches(const EventData& selector, const EventData& candidate) {
    if(const auto* hash = std::get_if<uint32_t>(&selector)) {
        const auto* value = std::get_if<uint32_t>(&candidate);

        return value != nullptr && *value == *hash;
    }

    if(const auto* number = std::get_if<int>(&selector)) {
        if(const auto* value = std::get_if<int>(&candidate))
            return *value == *number;

        const auto* unsigned_value = std::get_if<uint32_t>(&candidate);

        return unsigned_value != nullptr && static_cast<int>(*unsigned_value) == *number;
    }

    const auto* flag = std::get_if<bool>(&selector);
    const auto* value = std::get_if<bool>(&candidate);

    return flag != nullptr && value != nullptr && *value == *flag;
}

AnySubscription SubscribeEventBinding(const EventBinding& binding, EventBindingHandler handler) {
    auto* channel = EventChannelRegistry::GetInstance().Find(binding.channel);
    if(channel == nullptr || !handler)
        return nullptr;

    // The selector goes to the channel, not into the handler: a subscriber bound to another
    // sensor is then rejected under the subscriber lock instead of waking for every sample.
    ErasedEventFilter filter;
    if(binding.HasSelector()) {
        auto selector = ToSelectorValue(binding.selector_value);
        // Unfiltered, it would deliver every sensor's value; validators reject such a selector.
        if(!selector.has_value())
            return nullptr;

        filter = [selector_key = binding.selector_key, selector = *selector](const ErasedPayloadView& view) {
            const auto* candidate = view.Find(selector_key);

            return candidate != nullptr && SelectorMatches(selector, *candidate);
        };
    }

    return channel->SubscribeErased(
        binding.event_type,
        std::move(filter),
        [payload_key = binding.payload_key, handler = std::move(handler)](const ErasedPayloadView& view) {
            if(const auto* value = view.Find(payload_key))
                handler(*value);
        });
}

} // namespace eerie_leap::subsys::event_bus
