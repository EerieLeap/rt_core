#pragma once

#include <cstdint>

namespace eerie_leap::subsys::canbus {

// Persisted IDs: append new types before COUNT; never reorder or reuse values.
enum class CanbusType : uint8_t {
    NONE = 0,
    CLASSICAL_CAN,
    CANFD,
    COUNT // Sentinel, not a CAN bus type
};

constexpr bool IsCanbusTypeValid(CanbusType type) {
    return type < CanbusType::COUNT;
}

}  // namespace eerie_leap::subsys::canbus
