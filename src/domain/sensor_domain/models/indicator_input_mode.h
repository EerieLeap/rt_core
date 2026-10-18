#pragma once

#include <cstdint>

namespace eerie_leap::domain::sensor_domain::models {

// How an indicator's conditioned input becomes its value.
// Persisted IDs: append new modes before Count; never reorder or reuse values.
enum class IndicatorInputMode : std::uint32_t {
    Level = 0, // The value follows the conditioned input: a switch, ignition, "moving".
    Toggle,    // Each conditioned 0 -> 1 flips the value, which holds: a momentary button.
    Count      // Sentinel, not a mode
};

constexpr bool IsIndicatorInputModeValid(IndicatorInputMode mode) {
    return mode < IndicatorInputMode::Count;
}

} // namespace eerie_leap::domain::sensor_domain::models
