#include <stdexcept>
#include <string>
#include <string_view>

#include "subsys/event_bus/event_binding.h"
#include "subsys/event_bus/event_channel_id.h"

#include "logging_control_configuration_validator.h"

namespace eerie_leap::domain::logging_domain::control::configuration::parsers {

using eerie_leap::subsys::event_bus::IsSelectorValue;
using eerie_leap::subsys::event_bus::IsValidEventChannelId;

namespace {

void InvalidLoggingControlConfiguration(std::string_view message) {
    throw std::invalid_argument("Invalid logging control configuration. " + std::string(message));
}

} // namespace

void LoggingControlConfigurationValidator::Validate(const LoggingControlConfiguration& configuration) {
    if(configuration.triggers.size() > LoggingControlConfiguration::kMaxTriggers) {
        InvalidLoggingControlConfiguration(
            "At most " + std::to_string(LoggingControlConfiguration::kMaxTriggers) + " triggers."
        );
    }

    for(size_t i = 0; i < configuration.triggers.size(); i++) {
        const auto& binding = configuration.triggers[i].binding;

        if(!IsValidEventChannelId(binding.channel))
            InvalidLoggingControlConfiguration("Trigger " + std::to_string(i) + " names an unknown event channel.");

        // The event type, the payload key and the selector key are enums of the channel, which is
        // not known here.
        if(binding.HasSelector() && !IsSelectorValue(binding.selector_value)) {
            InvalidLoggingControlConfiguration(
                "Trigger " + std::to_string(i) + " selector value must be text, an integer or a bool."
            );
        }
    }
}

} // namespace eerie_leap::domain::logging_domain::control::configuration::parsers
