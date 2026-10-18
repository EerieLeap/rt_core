#pragma once

#include <cstdint>

namespace eerie_leap::domain::sensor_domain::models {

// Position of a sensor in the readings frame for the current configuration generation.
struct SensorSlot {
    static constexpr uint16_t INVALID = UINT16_MAX;

    uint16_t index = INVALID;

    [[nodiscard]] constexpr bool IsValid() const {
        return index != INVALID;
    }

    constexpr bool operator==(const SensorSlot&) const = default;
};

} // namespace eerie_leap::domain::sensor_domain::models
