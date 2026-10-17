#include <utility>
#include <variant>

#include "utilities/cbor/cbor_helpers.hpp"
#include "utilities/memory/memory_resource_manager.h"

#include "logging_control_configuration_cbor_parser.h"
#include "logging_control_configuration_validator.h"

namespace eerie_leap::domain::logging_domain::control::configuration::parsers {

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using eerie_leap::utilities::cbor::CborHelpers;
using eerie_leap::subsys::event_bus::EventChannelId;
using eerie_leap::domain::logging_domain::control::models::LoggingTrigger;

// The schema (logging_control_config.cddl) and the codec cap the triggers at 16; the validator enforces the model's cap.
static_assert(LoggingControlConfiguration::kMaxTriggers == 16, "The schema and the codec cap triggers at 16.");

pmr_unique_ptr<CborLoggingControlConfig> LoggingControlConfigurationCborParser::Serialize(
    const LoggingControlConfiguration& configuration) {

    LoggingControlConfigurationValidator::Validate(configuration);

    auto cbor_config = make_unique_pmr<CborLoggingControlConfig>(Mrm::GetExtPmr());

    for(const auto& trigger : configuration.triggers) {
        const auto& binding = trigger.binding;

        CborLoggingTriggerConfig cbor_trigger {
            .channel = static_cast<uint32_t>(binding.channel),
            .event_type = binding.event_type,
            .payload_key = binding.payload_key,
            .has_selector = binding.HasSelector(),
            .selector_key = binding.selector_key,
            .selector_value = {},
            .target_uid = trigger.target_uid
        };

        // Without a selector the stored value is int 0, which the decoder ignores.
        if(const auto* text = std::get_if<std::pmr::string>(&binding.selector_value))
            cbor_trigger.selector_value = CborHelpers::ToZcborString(*text);
        else if(const auto* number = std::get_if<int>(&binding.selector_value))
            cbor_trigger.selector_value = static_cast<int32_t>(*number);
        else if(const auto* flag = std::get_if<bool>(&binding.selector_value))
            cbor_trigger.selector_value = *flag;

        cbor_config->CborLoggingTriggerConfig_m.push_back(std::move(cbor_trigger));
    }

    return cbor_config;
}

pmr_unique_ptr<LoggingControlConfiguration> LoggingControlConfigurationCborParser::Deserialize(
    std::pmr::memory_resource* mr, const CborLoggingControlConfig& config) {

    auto configuration = make_unique_pmr<LoggingControlConfiguration>(mr);

    for(const auto& cbor_trigger : config.CborLoggingTriggerConfig_m) {
        LoggingTrigger trigger;
        trigger.binding.channel = static_cast<EventChannelId>(cbor_trigger.channel);
        trigger.binding.event_type = cbor_trigger.event_type;
        trigger.binding.payload_key = cbor_trigger.payload_key;
        trigger.binding.selector_key = cbor_trigger.selector_key;
        trigger.target_uid = cbor_trigger.target_uid;

        // Whatever is stored without a selector is dropped, so an absent selector is one value.
        if(cbor_trigger.has_selector) {
            if(const auto* text = std::get_if<zcbor_string>(&cbor_trigger.selector_value))
                trigger.binding.selector_value = CborHelpers::ToPmrString(mr, *text);
            else if(const auto* number = std::get_if<int32_t>(&cbor_trigger.selector_value))
                trigger.binding.selector_value = static_cast<int>(*number);
            else if(const auto* flag = std::get_if<bool>(&cbor_trigger.selector_value))
                trigger.binding.selector_value = *flag;
        }

        configuration->triggers.push_back(std::move(trigger));
    }

    LoggingControlConfigurationValidator::Validate(*configuration);

    return configuration;
}

} // namespace eerie_leap::domain::logging_domain::control::configuration::parsers
