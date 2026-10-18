#include <algorithm>
#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <zephyr/logging/log.h>

#include "subsys/event_bus/event.h"
#include "subsys/event_bus/event_binding_subscription.h"

#include "logging_trigger_set.h"

namespace eerie_leap::domain::logging_domain::control {

using eerie_leap::subsys::event_bus::EventData;
using eerie_leap::subsys::event_bus::SubscribeEventBinding;

LOG_MODULE_REGISTER(logging_trigger_set_logger);

namespace {

// A bool, or a number that is exactly 0 or 1: indicator sensors publish 0.0 and 1.0. Anything else,
// NaN and text included, is not a switch position.
std::optional<bool> ReadState(const EventData& value) {
    if(const auto* flag = std::get_if<bool>(&value))
        return *flag;

    if(const auto* number = std::get_if<int>(&value))
        return *number == 0 || *number == 1 ? std::optional<bool>(*number == 1) : std::nullopt;

    if(const auto* unsigned_number = std::get_if<uint32_t>(&value))
        return *unsigned_number == 0 || *unsigned_number == 1 ? std::optional<bool>(*unsigned_number == 1)
                                                              : std::nullopt;

    if(const auto* real = std::get_if<float>(&value))
        return *real == 0.0F || *real == 1.0F ? std::optional<bool>(*real == 1.0F) : std::nullopt;

    return std::nullopt;
}

} // namespace

struct LoggingTriggerSet::Trigger {
    // Valid while the guard is attached; the set holds the control until after it detaches.
    ILoggingControl* logging_control;
    size_t index;
    // What it selects, for the log lines and for OnConfigurationUpdated(). Read-only after construction.
    EventBinding binding;
    // The unit it starts and stops; ILoggingControl::ALL_UNITS is every unit that can log.
    uint32_t target_uid;

    // Inside the guard only: the guard serializes every delivery to the set, so a trigger reached
    // from two threads at once (the bus thread and a synchronous Publish()) never races on them.
    // Unknown until the first readable value.
    std::optional<bool> last_state;
    FirstValue first_value;
    bool is_unreadable_logged = false;
    // Set by OnConfigurationUpdated(): the sensor is gone, and what it still delivers is the
    // generation that is going.
    bool is_withdrawn = false;

    void OnValue(const EventData& value) {
        if(is_withdrawn)
            return;

        const auto state = ReadState(value);
        if(!state.has_value()) {
            // Once per trigger: a sensor that sends such values would flood the log. The last state
            // stays, as the value is no change.
            if(!is_unreadable_logged) {
                is_unreadable_logged = true;
                LOG_WRN(
                    "Logging trigger %u (channel %u) ignores a value that is not 0, 1 or a bool.",
                    static_cast<unsigned>(index), static_cast<unsigned>(binding.channel)
                );
            }

            return;
        }

        const auto previous = std::exchange(last_state, state);
        if(previous == state)
            return;

        // At boot the first value counts only when it is 1: a switch that is already on starts logging,
        // one that is off asserts nothing, so it does not stop a session another trigger or unit
        // started. After a configuration update it is a baseline, whatever it is: the update stopped
        // logging, and only a later 0 -> 1 starts it again.
        if(!previous.has_value() && (first_value == FirstValue::IsBaseline || !*state))
            return;

        if(*state)
            logging_control->RequestStart(target_uid);
        else
            logging_control->RequestStop(target_uid);
    }
};

LoggingTriggerSet::LoggingTriggerSet(
    std::shared_ptr<ILoggingControl> logging_control,
    std::span<const LoggingTrigger> triggers,
    const SelectorResolver& is_selector_known,
    FirstValue first_value
) :
    logging_control_(std::move(logging_control)),
    guard_(std::make_shared<DispatchGuard>(this)) {

    if(logging_control_ == nullptr)
        throw std::invalid_argument("The logging trigger set needs a logging control.");

    try {
        subscriptions_.reserve(triggers.size());
        triggers_.reserve(triggers.size());

        for(size_t i = 0; i < triggers.size(); i++) {
            const auto& binding = triggers[i].binding;

            if(is_selector_known && !is_selector_known(binding)) {
                LOG_WRN(
                    "Logging trigger %u (channel %u) selects nothing on this unit.", static_cast<unsigned>(i),
                    static_cast<unsigned>(binding.channel)
                );
            }

            auto trigger = std::make_shared<Trigger>(Trigger{ .logging_control = logging_control_.get(),
                .index = i,
                .binding = binding,
                .target_uid = triggers[i].target_uid,
                .last_state = std::nullopt,
                .first_value = first_value });

            // The handler holds the guard and the trigger's state, never the set.
            auto subscription = SubscribeEventBinding(binding, [guard = guard_, trigger](const EventData& value) {
                guard->Dispatch([&] {
                    trigger->OnValue(value);
                });
            });

            if(subscription == nullptr) {
                LOG_WRN(
                    "Logging trigger %u does nothing: channel %u is not registered on this unit, or its selector "
                    "cannot select.",
                    static_cast<unsigned>(i), static_cast<unsigned>(binding.channel)
                );
                continue;
            }

            subscriptions_.push_back(std::move(subscription));
            triggers_.push_back(std::move(trigger));
        }
    } catch(...) {
        // No handler may outlive a half-built set.
        guard_->Detach();
        subscriptions_.clear();
        triggers_.clear();

        throw;
    }
}

LoggingTriggerSet::~LoggingTriggerSet() {
    // A delivery the channel already took becomes a no-op, and one in its body is waited for;
    // neither requests anything afterwards.
    guard_->Detach();

    subscriptions_.clear();
    triggers_.clear();
}

size_t LoggingTriggerSet::GetSubscribedCount() const {
    return subscriptions_.size();
}

void LoggingTriggerSet::OnConfigurationUpdated(const SelectorResolver& is_selector_known) {
    if(triggers_.empty())
        return;

    // Outside the guard, on the caller's thread: the resolver reads the sensors configuration under
    // its manager's lock, and the bus thread that delivers a value must not wait for that.
    std::vector<bool> is_known(triggers_.size(), true);
    std::exception_ptr resolver_failure;
    if(is_selector_known) {
        try {
            for(size_t i = 0; i < triggers_.size(); i++)
                is_known[i] = is_selector_known(triggers_[i]->binding);
        } catch(...) {
            // Withdraws nothing, but the baselines below are set all the same.
            std::ranges::fill(is_known, true);
            resolver_failure = std::current_exception();
        }
    }
    const bool is_checked = is_selector_known && resolver_failure == nullptr;

    // Under the guard, which waits for a delivery in progress: from here on a trigger either saw the
    // value before the update or reads it as a baseline.
    enum class Change : uint8_t {
        None,
        Withdrawn,
        Restored
    };
    std::vector<Change> changes(triggers_.size(), Change::None);

    guard_->Dispatch([&] {
        for(size_t i = 0; i < triggers_.size(); i++) {
            auto& trigger = *triggers_[i];

            trigger.first_value = FirstValue::IsBaseline;

            if(!is_checked)
                continue;

            if(!is_known[i] && !trigger.is_withdrawn) {
                trigger.is_withdrawn = true;
                trigger.last_state.reset();
                changes[i] = Change::Withdrawn;
            } else if(is_known[i] && trigger.is_withdrawn) {
                trigger.is_withdrawn = false;
                changes[i] = Change::Restored;
            }
        }
    });

    for(size_t i = 0; i < changes.size(); i++) {
        const auto& trigger = *triggers_[i];

        if(changes[i] == Change::Withdrawn) {
            LOG_WRN(
                "Logging trigger %u (channel %u) selects nothing after the sensors update; it waits for its sensor.",
                static_cast<unsigned>(trigger.index), static_cast<unsigned>(trigger.binding.channel)
            );
        } else if(changes[i] == Change::Restored) {
            LOG_INF(
                "Logging trigger %u (channel %u) has its sensor again; its first value is a baseline.",
                static_cast<unsigned>(trigger.index), static_cast<unsigned>(trigger.binding.channel)
            );
        }
    }

    if(resolver_failure != nullptr)
        std::rethrow_exception(resolver_failure);
}

} // namespace eerie_leap::domain::logging_domain::control
