#pragma once

#include <variant>

#include "utilities/cbor/cbor_size_builder.hpp"

#include "cbor_logging_control_config.h"

using eerie_leap::utilities::cbor::CborSizeBuilder;

static size_t cbor_get_size_CborLoggingControlConfig(const CborLoggingControlConfig& config) {
    CborSizeBuilder builder;
    builder.AddIndefiniteArrayStart();

    builder.AddIndefiniteArrayStart();
    for(const auto& trigger : config.CborLoggingTriggerConfig_m) {
        builder.AddIndefiniteArrayStart()
            .AddUint(trigger.channel)
            .AddUint(trigger.event_type)
            .AddUint(trigger.payload_key)
            .AddBool(trigger.has_selector)
            .AddUint(trigger.selector_key);

        if(const auto* number = std::get_if<int32_t>(&trigger.selector_value))
            builder.AddInt(*number);
        else if(const auto* text = std::get_if<zcbor_string>(&trigger.selector_value))
            builder.AddTstr(*text);
        else if(const auto* flag = std::get_if<bool>(&trigger.selector_value))
            builder.AddBool(*flag);
    }

    return builder.Build();
}
