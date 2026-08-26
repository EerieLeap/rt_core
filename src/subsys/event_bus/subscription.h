#pragma once

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

#include "event.h"
#include "event_filter.h"

namespace eerie_leap::subsys::event_bus {

template<concepts::EnumClassUint32 EventTypeEnum, concepts::EnumClassUint32 PayloadTypeEnum>
struct Subscription {
    size_t id;
    EventTypeEnum event_type;
    std::function<bool(const Event<EventTypeEnum, PayloadTypeEnum>&)> filter;
    EventHandler<EventTypeEnum, PayloadTypeEnum> handler;

    template<EventFilter<EventTypeEnum, PayloadTypeEnum> FilterType>
    Subscription(size_t sub_id, EventTypeEnum type, FilterType f, EventHandler<EventTypeEnum, PayloadTypeEnum> h)
        : id(sub_id), event_type(type), filter(f), handler(std::move(h)) {}
};

// Shared, so dispatch can snapshot the matching subscribers by refcount instead of
// deep-copying their std::function handlers on every single event.
template<concepts::EnumClassUint32 EventTypeEnum, concepts::EnumClassUint32 PayloadTypeEnum>
using SubscriptionPtr = std::shared_ptr<Subscription<EventTypeEnum, PayloadTypeEnum>>;

template<concepts::EnumClassUint32 EventTypeEnum, concepts::EnumClassUint32 PayloadTypeEnum>
using SubscriberMap =
    std::unordered_map<EventTypeEnum, std::vector<SubscriptionPtr<EventTypeEnum, PayloadTypeEnum>>>;

} // namespace eerie_leap::subsys::event_bus
