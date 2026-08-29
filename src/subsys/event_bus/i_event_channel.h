#pragma once

#include <array>
#include <cstddef>

#include "i_event_bus.h"

namespace eerie_leap::subsys::event_bus {

// Type-erased view of an EventChannel, so a single bus can drain channels whose
// event and payload enums are unrelated.
class IEventChannel {
public:
    virtual ~IEventChannel() = default;

    // Diagnostics only, so a plain C string keeps it usable straight from LOG_*.
    virtual const char* GetName() const = 0;

    virtual const IEventBus* GetBus() const = 0;
    virtual void OnRegistered(IEventBus* bus) = 0;

    // Dispatches at most one queued event; reports whether one was dispatched.
    virtual bool DrainOne() = 0;
};

// Channels are singletons that outlive every bus, so the slots observe rather than
// own. A fixed array keeps the drain loop lock-free and allocation-free; registration
// only ever appends.
inline constexpr size_t k_max_bus_channels = 8;
using EventChannelSlots = std::array<IEventChannel*, k_max_bus_channels>;

} // namespace eerie_leap::subsys::event_bus
