#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "subsys/event_bus/dispatch_guard.h"
#include "subsys/event_bus/event_binding.h"
#include "subsys/event_bus/i_scoped_subscription.h"

#include "domain/logging_domain/control/models/logging_control_binding.h"
#include "i_logging_control.h"

namespace eerie_leap::domain::logging_domain::control {

using eerie_leap::domain::logging_domain::control::models::LoggingControlBinding;
using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::event_bus::DispatchGuard;
using eerie_leap::subsys::event_bus::EventBinding;

// The configured logging triggers, each subscribed to the value its binding names. A trigger acts on
// changes of that value read as a bool: 0 -> 1 requests a start and 1 -> 0 a stop. The first value it
// reads is special (FirstValue): at boot it counts when it is 1, so a switch that is already on starts
// logging; after a configuration update (L9) it is only a baseline, so nothing starts until a later
// 0 -> 1. Built from a configuration and replaced whole when the configuration changes: destroy the
// old set, build the new one, then ask the sensor events publisher to replay, so the new set sees the
// current value of its switch (which, after an update, makes its baseline current).
//
// The handlers run on the bus thread of their channel (the sensors' bus) and only call
// ILoggingControl, which never blocks. Build it after RemoteLoggingControl::Initialize(): a request
// before that is refused, and a switch that stays on asks no more.
//
// Every configuration stored on the unit stops logging (L9), and OnConfigurationUpdated() then makes
// the triggers take a baseline. A sensors update can also take a trigger's sensor away: the trigger is
// withdrawn, it forgets its last value and ignores values until a later update finds the sensor again,
// so the sensor's first value after that is a baseline too.
class LoggingTriggerSet {
public:
    // How a trigger takes the first value it reads, from unknown.
    enum class FirstValue : uint8_t {
        OneStarts,  // at boot (L3): 1 starts logging, 0 asserts nothing
        IsBaseline, // after a configuration update (L9): only the baseline; a later 0 -> 1 starts
    };

    // Whether the selector of a binding names something this unit has, such as a sensor. A false
    // answer is logged when the set is built; the trigger stays subscribed, so it acts as soon as a
    // sensor with that id publishes. Called while subscribing, on the building thread, and by
    // OnConfigurationUpdated(), on its caller's, outside the set's guard.
    using SelectorResolver = std::function<bool(const EventBinding& binding)>;

    // Subscribes every trigger. One whose channel this unit does not register, or whose selector
    // cannot select, is logged and does nothing. @p is_selector_known: null does not check.
    // @p first_value: IsBaseline for a set built after a configuration update.
    // Throws std::invalid_argument for a null control, and what SubscribeEventBinding() throws
    // (std::bad_alloc), having withdrawn what it subscribed.
    LoggingTriggerSet(
        std::shared_ptr<ILoggingControl> logging_control,
        std::span<const LoggingControlBinding> triggers,
        const SelectorResolver& is_selector_known = nullptr,
        FirstValue first_value = FirstValue::OneStarts
    );

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

    /**
     * A configuration was stored on this unit, which stops logging (L9). Every trigger now takes the
     * first value it reads as its baseline; one that knows its value keeps it, so nothing starts until
     * a later 0 -> 1. With @p is_selector_known (a sensors update), a trigger whose selector it no
     * longer accepts is withdrawn: it forgets its value and ignores values, the old generation's last
     * ones included, until a later call accepts the selector again; its first value then is a baseline
     * too. A trigger that selected nothing when the set was built is withdrawn by the first call that
     * still finds nothing. Requests nothing itself.
     *
     * The resolver runs first, outside the set's guard: it may take the sensors manager's lock, which
     * nothing holds while it takes the guard, and a delivery on the bus thread never waits for it.
     * Then one dispatch under the guard, which waits for a delivery in progress, sets the baselines and
     * the withdrawals. A resolver that throws withdraws nothing; the baselines are set all the same and
     * the exception is rethrown. Logs each trigger it withdraws (WRN) or restores (INF). One caller at
     * a time, not from a handler of the set and not while the set is destroyed (LoggingController calls
     * it inside its own guard).
     */
    void OnConfigurationUpdated(const SelectorResolver& is_selector_known = nullptr);

private:
    // Per-trigger state, shared with its handler.
    struct Trigger;

    std::shared_ptr<ILoggingControl> logging_control_;
    std::shared_ptr<DispatchGuard> guard_;
    std::vector<AnySubscription> subscriptions_;
    // The triggers that subscribed, for OnConfigurationUpdated(); the handlers share them. The vector
    // itself is fixed once the set is built.
    std::vector<std::shared_ptr<Trigger>> triggers_;
};

} // namespace eerie_leap::domain::logging_domain::control
