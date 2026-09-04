#pragma once

#include <memory>

namespace eerie_leap::subsys::event_bus {

class IScopedSubscription {
public:
    virtual ~IScopedSubscription() = default;
};

// Subscriptions to channels with unrelated event enums have unrelated handle types,
// so they can only share a container once the channel type is erased.
using AnySubscription = std::unique_ptr<IScopedSubscription>;

} // namespace eerie_leap::subsys::event_bus
