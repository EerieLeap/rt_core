#pragma once

#include <cstdint>

#include "sensor_type.h"

namespace eerie_leap::domain::sensor_domain::models {

enum class SensorSourceKind : uint8_t {
    NONE,
    ADC,          ///< An ADC channel, polled.
    GPIO,         ///< A GPIO channel, on edges or polled.
    CAN_SIGNAL,   ///< A signal decoded from a CAN frame.
    CAN_FRAME,    ///< A whole CAN frame.
    EXPRESSION,   ///< Computed from other sensors.
    SCRIPT,       ///< Produced by a Lua function.
};

enum class SensorValueKind : uint8_t {
    NONE,
    ANALOG,
    INDICATOR,    ///< Stored as 0 or 1.
};

// What each sensor type needs and allows. The validator, the reader factories and the processors
// read this table instead of enumerating types, so a new type is one row plus a source.
struct SensorTypeTraits {
    SensorSourceKind source = SensorSourceKind::NONE;
    SensorValueKind value_kind = SensorValueKind::NONE;
    bool requires_channel = false;         ///< ADC or GPIO channel.
    bool uses_canbus = false;              ///< Connection string names a bus and frame.
    bool requires_signal_name = false;     ///< Connection string also names a signal.
    bool requires_interpolation = false;   ///< Calibration table and method.
    bool requires_expression = false;
    bool allows_expression = true;
    bool requires_sampling_rate = false;
    bool allows_sampling_rate = true;
    bool has_input = false;                ///< The expression receives the reading as x.
    bool is_isr_driven = false;            ///< Updated by its source without a sampling rate.

    static constexpr SensorTypeTraits Of(SensorType type) {
        SensorTypeTraits traits;

        switch(type) {
        case SensorType::PHYSICAL_ANALOG:
            traits.source = SensorSourceKind::ADC;
            traits.value_kind = SensorValueKind::ANALOG;
            traits.requires_channel = true;
            traits.requires_interpolation = true;
            traits.requires_sampling_rate = true;
            traits.has_input = true;
            break;

        case SensorType::PHYSICAL_INDICATOR:
            traits.source = SensorSourceKind::GPIO;
            traits.value_kind = SensorValueKind::INDICATOR;
            traits.requires_channel = true;
            traits.has_input = true;
            traits.is_isr_driven = true;
            break;

        case SensorType::VIRTUAL_ANALOG:
            traits.source = SensorSourceKind::EXPRESSION;
            traits.value_kind = SensorValueKind::ANALOG;
            traits.requires_expression = true;
            traits.requires_sampling_rate = true;
            break;

        case SensorType::VIRTUAL_INDICATOR:
            traits.source = SensorSourceKind::EXPRESSION;
            traits.value_kind = SensorValueKind::INDICATOR;
            traits.requires_expression = true;
            traits.requires_sampling_rate = true;
            break;

        case SensorType::CANBUS_RAW:
            traits.source = SensorSourceKind::CAN_FRAME;
            traits.uses_canbus = true;
            traits.allows_expression = false;
            traits.allows_sampling_rate = false;
            traits.is_isr_driven = true;
            break;

        case SensorType::CANBUS_ANALOG:
            traits.source = SensorSourceKind::CAN_SIGNAL;
            traits.value_kind = SensorValueKind::ANALOG;
            traits.uses_canbus = true;
            traits.requires_signal_name = true;
            traits.allows_sampling_rate = false;
            traits.has_input = true;
            traits.is_isr_driven = true;
            break;

        case SensorType::CANBUS_INDICATOR:
            traits.source = SensorSourceKind::CAN_SIGNAL;
            traits.value_kind = SensorValueKind::INDICATOR;
            traits.uses_canbus = true;
            traits.requires_signal_name = true;
            traits.allows_sampling_rate = false;
            traits.has_input = true;
            traits.is_isr_driven = true;
            break;

        case SensorType::USER_ANALOG:
            traits.source = SensorSourceKind::SCRIPT;
            traits.value_kind = SensorValueKind::ANALOG;
            traits.requires_sampling_rate = true;
            traits.has_input = true;
            break;

        case SensorType::USER_INDICATOR:
            traits.source = SensorSourceKind::SCRIPT;
            traits.value_kind = SensorValueKind::INDICATOR;
            traits.requires_sampling_rate = true;
            traits.has_input = true;
            break;

        default:
            break;
        }

        return traits;
    }
};

} // namespace eerie_leap::domain::sensor_domain::models
