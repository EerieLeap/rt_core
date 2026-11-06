#pragma once

#include <cstdint>

namespace eerie_leap::domain::sensor_domain::models {

// Persisted IDs: append new types before COUNT; never reorder or reuse values. The names
// (GetSensorTypeName()) are persisted too, in the sensor_type property of every log channel: never rename.
enum class SensorType : std::uint32_t {
    NONE = 0,
    PHYSICAL_ANALOG,
    VIRTUAL_ANALOG,
    PHYSICAL_INDICATOR,
    VIRTUAL_INDICATOR,
    CANBUS_RAW,
    CANBUS_ANALOG,
    CANBUS_INDICATOR,
    USER_ANALOG,
    USER_INDICATOR,
    CONTROL_INDICATOR,
    CONTROL_ANALOG,
    COUNT // Sentinel, not a sensor type
};

constexpr bool IsSensorTypeValid(SensorType type) {
    return type < SensorType::COUNT;
}

// The enumerator's name, "UNKNOWN" for COUNT and for a value past it. No default case, so a type added
// to the enum is a -Wswitch warning until it is named here.
constexpr const char* GetSensorTypeName(SensorType type) {
    switch(type) {
        case SensorType::NONE:
            return "NONE";
        case SensorType::PHYSICAL_ANALOG:
            return "PHYSICAL_ANALOG";
        case SensorType::VIRTUAL_ANALOG:
            return "VIRTUAL_ANALOG";
        case SensorType::PHYSICAL_INDICATOR:
            return "PHYSICAL_INDICATOR";
        case SensorType::VIRTUAL_INDICATOR:
            return "VIRTUAL_INDICATOR";
        case SensorType::CANBUS_RAW:
            return "CANBUS_RAW";
        case SensorType::CANBUS_ANALOG:
            return "CANBUS_ANALOG";
        case SensorType::CANBUS_INDICATOR:
            return "CANBUS_INDICATOR";
        case SensorType::USER_ANALOG:
            return "USER_ANALOG";
        case SensorType::USER_INDICATOR:
            return "USER_INDICATOR";
        case SensorType::CONTROL_INDICATOR:
            return "CONTROL_INDICATOR";
        case SensorType::CONTROL_ANALOG:
            return "CONTROL_ANALOG";
        case SensorType::COUNT:
            break;
    }

    return "UNKNOWN";
}

} // namespace eerie_leap::domain::sensor_domain::models
