#pragma once

#include <atomic>
#include <cstddef>
#include <expected>
#include <memory>
#include <queue>
#include <string>
#include <utility>
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
    uint32_t last_dropped_source_id_ = 0;

    // Observes the registering bus; null is the inert state, and ~EventBus() restores
    // it so a destroyed bus can never be woken.
    std::atomic<IEventBus*> bus_{nullptr};

    // Borrows the dispatched event's payload, so an erased subscriber reads it without a copy
    // and without an allocation.
    class PayloadView final : public ErasedPayloadView {
    private:
        const PayloadMap& payload_;

    public:
        explicit PayloadView(const PayloadMap& payload) : payload_(payload) { }

        const EventData* Find(uint32_t key) const override {
            auto it = payload_.find(static_cast<TPayloadType>(key));

            return it == payload_.end() ? nullptr : &it->second;
        }
    };

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

    AnySubscription SubscribeErased(uint32_t event_type, ErasedEventHandler handler) override;

    void Publish(const EventMessage& event);
    void PublishAsync(const EventMessage& event);
};

template<concepts::EnumClassUint32 TEventType, concepts::EnumClassUint32 TPayloadType>
class ScopedSubscription final : public IScopedSubscription {
private:
    EventChannel<TEventType, TPayloadType>& channel_;
    SubscriptionHandle<TEventType> handle_;

public:
    ScopedSubscription(EventChannel<TEventType, TPayloadType>& channel, SubscriptionHandle<TEventType>&& handle)
        : channel_(channel), handle_(std::move(handle)) { }

    ~ScopedSubscription() override {
        channel_.Unsubscribe(handle_);
    }

    ScopedSubscription(const ScopedSubscription&) = delete;
    ScopedSubscription& operator=(const ScopedSubscription&) = delete;
};

// Returns nullptr when the channel refuses the subscription.
template<typename ChannelType, EventFilter<typename ChannelType::EventTypeEnum, typename ChannelType::PayloadTypeEnum> FilterType>
AnySubscription CreateScopedSubscription(
    ChannelType& channel,
    typename ChannelType::EventTypeEnum type,
    FilterType filter,
    EventHandler<typename ChannelType::EventTypeEnum, typename ChannelType::PayloadTypeEnum> handler) {

    auto subscription = channel.Subscribe(type, std::move(filter), std::move(handler));
    if(!subscription)
        return nullptr;

    return std::make_unique<ScopedSubscription<typename ChannelType::EventTypeEnum, typename ChannelType::PayloadTypeEnum>>(
        channel, std::move(*subscription));
}

template<typename ChannelType>
AnySubscription CreateScopedSubscription(
    ChannelType& channel,
    typename ChannelType::EventTypeEnum type,
    EventHandler<typename ChannelType::EventTypeEnum, typename ChannelType::PayloadTypeEnum> handler) {

    return CreateScopedSubscription(
        channel,
        type,
        AcceptAllFilter<typename ChannelType::EventTypeEnum, typename ChannelType::PayloadTypeEnum>{ },
        std::move(handler));
}

} // namespace eerie_leap::subsys::event_bus

#include "event_channel.tpp"
