#pragma once

#include <functional>
#include <optional>

#include "event.h"
#include "event_binding.h"
#include "i_scoped_subscription.h"

namespace eerie_leap::subsys::event_bus {

// Receives the value the binding's payload key carries. The reference is valid for the call only.
using EventBindingHandler = std::function<void(const EventData& value)>;

// Configuration names a selector by the readable id it shares with the rest of the system, and
// payloads carry text ids hashed, so text becomes its hash here, once, when the binding resolves.
// An integer or a bool is kept. Null for a value IsSelectorValue() rejects.
std::optional<EventData> ToSelectorValue(const ConfigValue& value);

// Whether a payload entry matches a resolved selector: a hash matches the same uint32, an integer
// the same int or uint32, a bool the same bool.
bool SelectorMatches(const EventData& selector, const EventData& candidate);

// Subscribes @p handler to the value @p binding names. The selector becomes the channel's filter,
// evaluated under its subscriber lock, so an event for another sensor never wakes the handler; an
// event without the payload key does not reach it either. The handler runs on the thread that
// dispatches: the bus worker for PublishAsync(), the publisher for Publish().
//
// Null for EventChannelId::None, a channel this unit did not register, a selector that cannot
// select, an empty handler, or a subscription the channel refuses: a stale binding is inert, not
// fatal, and the caller logs it with its own context. Destroying the result unsubscribes but does
// not wait for a delivery already under way (see DispatchGuard). May throw std::bad_alloc, like
// SubscribeErased(); call it while configuring, not on a hot path.
AnySubscription SubscribeEventBinding(const EventBinding& binding, EventBindingHandler handler);

} // namespace eerie_leap::subsys::event_bus
