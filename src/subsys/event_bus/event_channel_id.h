#pragma once

#include <cstdint>

namespace eerie_leap::subsys::event_bus {

// Persisted in stored configuration as the channel a binding names (widget bindings, logging
// triggers) - append only. One enum serves every application, so a stored value names the same
// channel on every unit. Navigation and Settings are gauge channels no other unit registers.
enum class EventChannelId : uint32_t {
    None = 0,
    Sensors = 1,    // sensor_events
    Logging = 2,    // logging_events
    Navigation = 3, // navigation_events, rt_gauge
    Settings = 4,   // settings_events, rt_gauge
};

// Appending an id means moving this to it; IsValidEventChannelId() and the registry's table follow.
inline constexpr EventChannelId LAST_EVENT_CHANNEL_ID = EventChannelId::Settings;

// Whether a stored id names a channel at all; whether this unit registers it is the registry's
// question.
constexpr bool IsValidEventChannelId(EventChannelId channel) {
    return channel != EventChannelId::None && channel <= LAST_EVENT_CHANNEL_ID;
}

} // namespace eerie_leap::subsys::event_bus
