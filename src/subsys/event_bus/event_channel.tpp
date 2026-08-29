#include <algorithm>
#include <optional>
#include <utility>

#include <zephyr/sys/printk.h>

#include "subsys/threading/scoped_mutex.h"

#include "event_channel.h"

namespace eerie_leap::subsys::event_bus {

using eerie_leap::subsys::threading::ScopedMutex;

template<concepts::EnumClassUint32 TEventType, concepts::EnumClassUint32 TPayloadType>
EventChannel<TEventType, TPayloadType>::EventChannel(std::string name, size_t max_queued_events)
    : name_(std::move(name)), max_queued_events_(max_queued_events) {

    k_mutex_init(&subscribers_mutex_);
    k_mutex_init(&queue_mutex_);
}

template<concepts::EnumClassUint32 TEventType, concepts::EnumClassUint32 TPayloadType>
template<EventFilter<TEventType, TPayloadType> FilterType>
std::expected<SubscriptionHandle<TEventType>, std::string>
EventChannel<TEventType, TPayloadType>::Subscribe(TEventType type, FilterType filter, EventHandler<TEventType, TPayloadType> handler) {
    ScopedMutex guard(subscribers_mutex_);

    try {
        size_t id = next_id_++;
        auto subscription = std::make_shared<Subscription<TEventType, TPayloadType>>(id, type, filter, std::move(handler));

        subscribers_[type].push_back(std::move(subscription));

        return SubscriptionHandle<TEventType>{id, type};
    } catch (const std::exception& e) {
        return std::unexpected("Subscription failed: " + std::string(e.what()));
    }
}

template<concepts::EnumClassUint32 TEventType, concepts::EnumClassUint32 TPayloadType>
bool EventChannel<TEventType, TPayloadType>::Unsubscribe(SubscriptionHandle<TEventType>& handle) {
    if(!handle.IsValid())
        return false;

    ScopedMutex guard(subscribers_mutex_);

    auto it = subscribers_.find(handle.GetEventType());
    if(it == subscribers_.end())
        return false;

    auto& subscribers = it->second;
    auto subscription_it = std::find_if(subscribers.begin(), subscribers.end(),
        [&handle](const auto& sub) {
            return sub->id == handle.GetId();
    });

    if(subscription_it != subscribers.end()) {
        subscribers.erase(subscription_it);
        handle.Invalidate();

        return true;
    }

    return false;
}

template<concepts::EnumClassUint32 TEventType, concepts::EnumClassUint32 TPayloadType>
void EventChannel<TEventType, TPayloadType>::Publish(const EventMessage& event) {
    if(!IsRegistered())
        return;

    Dispatch(event);
}

template<concepts::EnumClassUint32 TEventType, concepts::EnumClassUint32 TPayloadType>
void EventChannel<TEventType, TPayloadType>::PublishAsync(const EventMessage& event) {
    auto* bus = bus_.load(std::memory_order_acquire);
    if(bus == nullptr)
        return;

    {
        ScopedMutex guard(queue_mutex_);

        // Shed the oldest rather than the newest
        while(event_queue_.size() >= max_queued_events_) {
            event_queue_.pop();
            ++dropped_events_;
        }

        event_queue_.push(event);
    }

    bus->Wake();
}

template<concepts::EnumClassUint32 TEventType, concepts::EnumClassUint32 TPayloadType>
bool EventChannel<TEventType, TPayloadType>::DrainOne() {
    std::optional<EventMessage> event;
    size_t dropped = 0;

    {
        ScopedMutex guard(queue_mutex_);

        if(!event_queue_.empty()) {
            event = std::move(event_queue_.front());
            event_queue_.pop();
        }

        dropped = std::exchange(dropped_events_, 0);
    }

    if(dropped != 0)
        printk("[event_bus] channel '%s' dropped %u queued events, subscribers cannot keep up\n",
            name_.c_str(), static_cast<unsigned>(dropped));

    if(!event)
        return false;

    Dispatch(*event);

    return true;
}

template<concepts::EnumClassUint32 TEventType, concepts::EnumClassUint32 TPayloadType>
void EventChannel<TEventType, TPayloadType>::Dispatch(const EventMessage& event) {
    // Snapshot under the lock: a handler may subscribe or unsubscribe, which would
    // otherwise mutate the very vector being iterated.
    std::vector<SubscriptionPtr<TEventType, TPayloadType>> matched;

    {
        ScopedMutex guard(subscribers_mutex_);

        if(auto it = subscribers_.find(event.type); it != subscribers_.end()) {
            matched.reserve(it->second.size());

            for(const auto& subscription : it->second) {
                if(subscription->filter(event))
                    matched.push_back(subscription);
            }
        }
    }

    for(const auto& subscription : matched) {
        try {
            subscription->handler(event);
        } catch (const std::exception& e) {
            printk("[event_bus] channel '%s' subscriber threw for event type %u: %s\n",
                name_.c_str(), static_cast<unsigned>(event.type), e.what());
        } catch (...) {
            printk("[event_bus] channel '%s' subscriber threw a non-standard exception for event type %u\n",
                name_.c_str(), static_cast<unsigned>(event.type));
        }
    }
}

} // namespace eerie_leap::subsys::event_bus
