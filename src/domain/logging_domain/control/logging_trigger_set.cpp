#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include <zephyr/logging/log.h>

#include "subsys/event_bus/event.h"
#include "subsys/event_bus/event_binding_subscription.h"

#include "logging_trigger_set.h"

namespace eerie_leap::domain::logging_domain::control {

using eerie_leap::subsys::event_bus::EventChannelId;
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
        return *unsigned_number == 0 || *unsigned_number == 1 ? std::optional<bool>(*unsigned_number == 1) : std::nullopt;

    if(const auto* real = std::get_if<float>(&value))
        return *real == 0.0f || *real == 1.0f ? std::optional<bool>(*real == 1.0f) : std::nullopt;

    return std::nullopt;
}

} // namespace

struct LoggingTriggerSet::Trigger {
    // Valid while the guard is attached; the set holds the control until after it detaches.
    ILoggingControl* logging_control;
    size_t index;
    // For the log line.
    EventChannelId channel;

    // Inside the guard only: the guard serializes every delivery to the set, so a trigger reached
    // from two threads at once (the bus thread and a synchronous Publish()) never races on them.
    // Unknown until the first readable value.
    std::optional<bool> last_state;
    bool is_unreadable_logged = false;

    void OnValue(const EventData& value) {
        const auto state = ReadState(value);
        if(!state.has_value()) {
            // Once per trigger: a sensor that sends such values would flood the log. The last state
            // stays, as the value is no change.
            if(!is_unreadable_logged) {
                is_unreadable_logged = true;
                LOG_WRN("Logging trigger %u (channel %u) ignores a value that is not 0, 1 or a bool.",
                    static_cast<unsigned>(index), static_cast<unsigned>(channel));
            }

            return;
        }

        const auto previous = std::exchange(last_state, state);
        if(previous == state)
            return;

        // The first value counts only when it is 1: a switch that is already on starts logging, one
        // that is off asserts nothing, so it does not stop a session another trigger or unit started.
        if(!previous.has_value() && !*state)
            return;

        if(*state)
            logging_control->RequestStart();
        else
            logging_control->RequestStop();
    }
};

LoggingTriggerSet::LoggingTriggerSet(
    std::shared_ptr<ILoggingControl> logging_control,
    std::span<const LoggingTrigger> triggers,
    SelectorResolver is_selector_known)
        : logging_control_(std::move(logging_control)),
        guard_(std::make_shared<DispatchGuard>(this)) {

    if(logging_control_ == nullptr)
        throw std::invalid_argument("The logging trigger set needs a logging control.");

    try {
        subscriptions_.reserve(triggers.size());

        for(size_t i = 0; i < triggers.size(); i++) {
            const auto& binding = triggers[i].binding;

            if(is_selector_known && !is_selector_known(binding)) {
                LOG_WRN("Logging trigger %u (channel %u) selects nothing on this unit.",
                    static_cast<unsigned>(i), static_cast<unsigned>(binding.channel));
            }

            auto trigger = std::make_shared<Trigger>(Trigger {
                .logging_control = logging_control_.get(),
                .index = i,
                .channel = binding.channel
            });

            // The handler holds the guard and the trigger's state, never the set.
            auto subscription = SubscribeEventBinding(
                binding,
                [guard = guard_, trigger = std::move(trigger)](const EventData& value) {
                    guard->Dispatch([&] { trigger->OnValue(value); });
                });

            if(subscription == nullptr) {
                LOG_WRN("Logging trigger %u does nothing: channel %u is not registered on this unit, or its selector cannot select.",
                    static_cast<unsigned>(i), static_cast<unsigned>(binding.channel));
                continue;
            }

            subscriptions_.push_back(std::move(subscription));
        }
    } catch(...) {
        // No handler may outlive a half-built set.
        guard_->Detach();
        subscriptions_.clear();

        throw;
    }
}

LoggingTriggerSet::~LoggingTriggerSet() {
    // A delivery the channel already took becomes a no-op, and one in its body is waited for;
    // neither requests anything afterwards.
    guard_->Detach();

    subscriptions_.clear();
}

size_t LoggingTriggerSet::GetSubscribedCount() const {
    return subscriptions_.size();
}

} // namespace eerie_leap::domain::logging_domain::control
