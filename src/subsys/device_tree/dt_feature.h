#pragma once

#include <cstdint>

namespace eerie_leap::subsys::device_tree {

enum class DtFeature : uint32_t {
    NONE = 0,
    INTERNAL_FS = 1 << 0,
    SD_FS = 1 << 1,
    GPIO = 1 << 2,
    ADC = 1 << 3,
    DISPLAY = 1 << 4,
    CANBUS = 1 << 5,
    ALL = INTERNAL_FS | SD_FS | GPIO | ADC | DISPLAY | CANBUS
};

constexpr uint32_t DtFeatureValue(DtFeature value) {
    return static_cast<uint32_t>(value);
}

constexpr DtFeature operator|(DtFeature lhs, DtFeature rhs) {
    return static_cast<DtFeature>(DtFeatureValue(lhs) | DtFeatureValue(rhs));
}

constexpr DtFeature operator&(DtFeature lhs, DtFeature rhs) {
    return static_cast<DtFeature>(DtFeatureValue(lhs) & DtFeatureValue(rhs));
}

constexpr DtFeature operator~(DtFeature value) {
    return static_cast<DtFeature>(~DtFeatureValue(value)) & DtFeature::ALL;
}

constexpr DtFeature& operator|=(DtFeature& lhs, DtFeature rhs) {
    lhs = lhs | rhs;
    return lhs;
}

constexpr DtFeature& operator&=(DtFeature& lhs, DtFeature rhs) {
    lhs = lhs & rhs;
    return lhs;
}

constexpr bool HasDtFeature(DtFeature features, DtFeature feature) {
    return (features & feature) == feature;
}

} // namespace eerie_leap::subsys::device_tree
