#pragma once

#include <array>
#include <cstddef>
#include <utility>

#include "event_channel_id.h"
#include "i_event_channel.h"

namespace eerie_leap::subsys::event_bus {

// Resolves the channel id a stored binding names (a widget's, a logging trigger's) onto the channel
// itself. Each application registers the channels it carries while it composes, before anything
// resolves a binding; Find() takes no lock. Observes rather than owns: every channel is a singleton
// that outlives the registry.
class EventChannelRegistry {
private:
    // Sized from the last id, so every id IsValidEventChannelId() accepts has a slot.
    static constexpr size_t K_CHANNEL_COUNT = std::to_underlying(LAST_EVENT_CHANNEL_ID) + 1;

    std::array<IEventChannel*, K_CHANNEL_COUNT> channels_{};

    EventChannelRegistry() = default;

public:
    static EventChannelRegistry& GetInstance();

    EventChannelRegistry(const EventChannelRegistry&) = delete;
    EventChannelRegistry& operator=(const EventChannelRegistry&) = delete;
    ~EventChannelRegistry() = default;

    // 0, also for the channel already held under @p id; -EINVAL for None or an id that names no
    // channel (IsValidEventChannelId), -EEXIST for a second channel under a taken id.
    int Register(EventChannelId id, IEventChannel& channel);

    // Null for an id nothing was registered under, so a stale binding is inert rather than fatal.
    IEventChannel* Find(EventChannelId id) const;
};

} // namespace eerie_leap::subsys::event_bus
