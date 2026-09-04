#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include "erased_payload_view.h"
#include "i_event_bus.h"
#include "i_scoped_subscription.h"

namespace eerie_leap::subsys::event_bus {

// Payload of an erased publish. A span rather than a map: the caller knows its keys at the call
// site and can build them on the stack.
using ErasedPayload = std::span<const std::pair<uint32_t, EventData>>;

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

    // Subscribes without naming the channel's enums, so a binding held as configuration can
    // resolve to a subscription at runtime. Both enums are uint32-backed by concept.
    virtual AnySubscription SubscribeErased(
        uint32_t event_type, ErasedEventFilter filter, ErasedEventHandler handler) = 0;

    virtual void PublishErasedAsync(uint32_t event_type, uint32_t source_id, ErasedPayload payload) = 0;
};

// Channels are singletons that outlive every bus, so the slots observe rather than
// own. A fixed array keeps the drain loop lock-free and allocation-free; registration
// only ever appends.
inline constexpr size_t k_max_bus_channels = 8;
using EventChannelSlots = std::array<IEventChannel*, k_max_bus_channels>;

} // namespace eerie_leap::subsys::event_bus
