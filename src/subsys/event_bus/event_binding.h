#pragma once

#include <cstdint>
#include <functional>
#include <memory_resource>
#include <string>
#include <string_view>
#include <variant>

#include "utilities/type/config_value.h"

#include "event_channel_id.h"

namespace eerie_leap::subsys::event_bus {

using eerie_leap::utilities::type::ConfigValue;

// Names one value on the bus the way stored configuration does: a channel id and the raw values of
// that channel's enums, so it can be stored and resolved without the reader knowing the channel.
// Widget bindings and logging triggers carry one. Header only: configuration parsers and validators
// use it in builds without the event bus.
struct EventBinding {
    EventChannelId channel = EventChannelId::None;

    // Channel-local enum values, erased to match IEventChannel::SubscribeErased.
    uint32_t event_type = 0;
    uint32_t payload_key = 0;

    // Narrows the subscription to one sensor or one setting. The key cannot signal absence on its
    // own, because 0 is a live payload key on every channel, so an unset value carries that.
    uint32_t selector_key = 0;
    ConfigValue selector_value;

    bool HasSelector() const {
        return !std::holds_alternative<std::monostate>(selector_value);
    }
};

// Whether a value can select: text (matched by its hash), an integer or a bool. Validators check
// stored selectors with it; ToSelectorValue() resolves exactly these.
inline bool IsSelectorValue(const ConfigValue& value) {
    return std::holds_alternative<std::pmr::string>(value)
        || std::holds_alternative<int>(value)
        || std::holds_alternative<bool>(value);
}

// Why this unit could not resolve a binding, or empty when it can: GetEventBindingError()
// (event_binding_subscription.h) on a unit with the event bus. The managers that store bindings
// take one from their controller to check a configuration a client applies, so their domains still
// link without the event bus.
using EventBindingChecker = std::function<std::string_view(const EventBinding& binding)>;

} // namespace eerie_leap::subsys::event_bus
