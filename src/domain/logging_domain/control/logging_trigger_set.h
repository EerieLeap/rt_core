#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "subsys/event_bus/dispatch_guard.h"
#include "subsys/event_bus/event_binding.h"
#include "subsys/event_bus/i_scoped_subscription.h"

#include "domain/logging_domain/control/models/logging_trigger.h"
#include "i_logging_control.h"

namespace eerie_leap::domain::logging_domain::control {

using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::event_bus::DispatchGuard;
using eerie_leap::subsys::event_bus::EventBinding;
using eerie_leap::domain::logging_domain::control::models::LoggingTrigger;

// The configured logging triggers, each subscribed to the value its binding names. A trigger acts on
// changes of that value read as a bool: the first value counts only when it is 1 (start); after it,
// 0 -> 1 requests a start and 1 -> 0 a stop. Built from a configuration and replaced whole when the
// configuration changes: destroy the old set, build the new one, then ask the sensor events
// publisher to replay, so the new set sees the current value of its switch.
//
// The handlers run on the bus thread of their channel (the sensors' bus) and only call
// ILoggingControl, which never blocks. Build it after RemoteLoggingControl::Initialize(): a request
// before that is refused, and a switch that stays on asks no more.
class LoggingTriggerSet {
public:
    // Whether the selector of a binding names something this unit has, such as a sensor. A false
    // answer is logged when the set is built; the trigger stays subscribed, so it acts as soon as a
    // sensor with that id publishes. Called while subscribing, on the building thread.
    using SelectorResolver = std::function<bool(const EventBinding& binding)>;

    // Subscribes every trigger. One whose channel this unit does not register, or whose selector
    // cannot select, is logged and does nothing. @p is_selector_known: null does not check.
    // Throws std::invalid_argument for a null control, and what SubscribeEventBinding() throws
    // (std::bad_alloc), having withdrawn what it subscribed.
    LoggingTriggerSet(
        std::shared_ptr<ILoggingControl> logging_control,
        std::span<const LoggingTrigger> triggers,
        SelectorResolver is_selector_known = nullptr);

    // Detaches the guard, waiting for a handler in progress, then unsubscribes: a delivery the bus
    // already took becomes a no-op. Not from inside a trigger's handler.
    ~LoggingTriggerSet();

    // The handlers refer to the set's guard, not to the set.
    LoggingTriggerSet(const LoggingTriggerSet&) = delete;
    LoggingTriggerSet& operator=(const LoggingTriggerSet&) = delete;
    LoggingTriggerSet(LoggingTriggerSet&&) = delete;
    LoggingTriggerSet& operator=(LoggingTriggerSet&&) = delete;

    // How many triggers subscribed; the others do nothing.
    [[nodiscard]] size_t GetSubscribedCount() const;

private:
    // Per-trigger state, shared with its handler.
    struct Trigger;

    std::shared_ptr<ILoggingControl> logging_control_;
    std::shared_ptr<DispatchGuard> guard_;
    std::vector<AnySubscription> subscriptions_;
};

} // namespace eerie_leap::domain::logging_domain::control
