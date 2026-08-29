#pragma once

#include <atomic>
#include <cstddef>
#include <expected>
#include <queue>
#include <string>
#include <vector>

#include <zephyr/kernel.h>

#include "event.h"
#include "event_filter.h"
#include "i_event_bus.h"
#include "i_event_channel.h"
#include "subscription.h"
#include "subscription_handle.h"

namespace eerie_leap::subsys::event_bus {

namespace concepts = eerie_leap::utilities::concepts;

// Owns the subscribers and the pending queue for one family of events. A channel is
// inert until a bus registers it: publishing is dropped while subscribing still works,
// so a domain can hand out a channel that the composition root may never wire up.
template<concepts::EnumClassUint32 TEventType, concepts::EnumClassUint32 TPayloadType>
class EventChannel : public IEventChannel {
public:
    using EventTypeEnum = TEventType;
    using PayloadTypeEnum = TPayloadType;
    using EventMessage = Event<TEventType, TPayloadType>;
    using PayloadMap = EventPayload<TPayloadType>;
    using HandleType = SubscriptionHandle<TEventType>;

    static constexpr size_t k_default_max_queued_events = 64;

private:
    std::string name_;
    size_t max_queued_events_;

    SubscriberMap<TEventType, TPayloadType> subscribers_;
    k_mutex subscribers_mutex_;
    size_t next_id_ = 1;

    std::queue<EventMessage> event_queue_;
    k_mutex queue_mutex_;
    size_t dropped_events_ = 0;

    // Observes the registering bus; null is the inert state, and ~EventBus() restores
    // it so a destroyed bus can never be woken.
    std::atomic<IEventBus*> bus_{nullptr};

    void Dispatch(const EventMessage& event);

protected:
    EventChannel(std::string name, size_t max_queued_events = k_default_max_queued_events);

public:
    ~EventChannel() override = default;

    EventChannel(const EventChannel&) = delete;
    EventChannel& operator=(const EventChannel&) = delete;

    const char* GetName() const override { return name_.c_str(); }
    const IEventBus* GetBus() const override { return bus_.load(std::memory_order_acquire); }
    void OnRegistered(IEventBus* bus) override { bus_.store(bus, std::memory_order_release); }
    bool IsRegistered() const { return GetBus() != nullptr; }

    bool DrainOne() override;

    template<EventFilter<TEventType, TPayloadType> FilterType = AcceptAllFilter<TEventType, TPayloadType>>
    std::expected<SubscriptionHandle<TEventType>, std::string>
    Subscribe(TEventType type, FilterType filter, EventHandler<TEventType, TPayloadType> handler);

    std::expected<SubscriptionHandle<TEventType>, std::string>
    Subscribe(TEventType type, EventHandler<TEventType, TPayloadType> handler) {
        return Subscribe(type, AcceptAllFilter<TEventType, TPayloadType>{ }, std::move(handler));
    }

    bool Unsubscribe(SubscriptionHandle<TEventType>& handle);

    void Publish(const EventMessage& event);
    void PublishAsync(const EventMessage& event);
};

} // namespace eerie_leap::subsys::event_bus

#include "event_channel.tpp"
