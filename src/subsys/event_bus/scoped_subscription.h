#pragma once

#include <memory>
#include <utility>

#include "event_channel.h"
#include "subscription_handle.h"

namespace eerie_leap::subsys::event_bus {

class IScopedSubscription {
public:
    virtual ~IScopedSubscription() = default;
};

// Subscriptions to channels with unrelated event enums have unrelated handle types,
// so they can only share a container once the channel type is erased.
using AnySubscription = std::unique_ptr<IScopedSubscription>;

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
