#include <stdexcept>
#include <string>
#include <string_view>

#include "subsys/event_bus/event_binding.h"
#include "subsys/event_bus/event_channel_id.h"

#include "domain/sensor_domain/event_bus/sensor_event_binding.h"

#include "logging_control_configuration_validator.h"

namespace eerie_leap::domain::logging_domain::control::configuration::parsers {

using eerie_leap::domain::sensor_domain::event_bus::SelectsSensor;
using eerie_leap::subsys::event_bus::EventChannelId;
using eerie_leap::subsys::event_bus::IsSelectorValue;
using eerie_leap::subsys::event_bus::IsValidEventChannelId;

namespace {

void InvalidLoggingControlConfiguration(std::string_view message) {
    throw std::invalid_argument("Invalid logging control configuration. " + std::string(message));
}

} // namespace

void LoggingControlConfigurationValidator::Validate(const LoggingControlConfiguration& configuration) {
    if(configuration.triggers.size() > LoggingControlConfiguration::MAX_TRIGGERS) {
        InvalidLoggingControlConfiguration(
            "At most " + std::to_string(LoggingControlConfiguration::MAX_TRIGGERS) + " triggers."
        );
    }

    for(size_t i = 0; i < configuration.triggers.size(); i++) {
        const auto& binding = configuration.triggers[i].event;

        if(!IsValidEventChannelId(binding.channel))
            InvalidLoggingControlConfiguration("Trigger " + std::to_string(i) + " names an unknown event channel.");

        // A trigger on a sensor must select one: without a selector it would receive every sensor's
        // value and make the publisher publish every reading.
        if(binding.channel == EventChannelId::Sensors && !SelectsSensor(binding)) {
            InvalidLoggingControlConfiguration(
                "Trigger " + std::to_string(i) + " on the Sensors channel must select a sensor by its SensorId."
            );
        }

        // The event type, the payload key and the selector key are enums of the channel, which is
        // not known here; ValidateBindings() asks the unit applying the configuration.
        if(binding.HasSelector() && !IsSelectorValue(binding.selector_value)) {
            InvalidLoggingControlConfiguration(
                "Trigger " + std::to_string(i) + " selector value must be text, an integer or a bool."
            );
        }
    }
}

void LoggingControlConfigurationValidator::ValidateBindings(
    const LoggingControlConfiguration& configuration,
    const EventBindingChecker& check_binding
) {
    if(!check_binding)
        return;

    for(size_t i = 0; i < configuration.triggers.size(); i++) {
        const auto reason = check_binding(configuration.triggers[i].event);
        if(!reason.empty())
            InvalidLoggingControlConfiguration("Trigger " + std::to_string(i) + " " + std::string(reason));
    }
}

} // namespace eerie_leap::domain::logging_domain::control::configuration::parsers
