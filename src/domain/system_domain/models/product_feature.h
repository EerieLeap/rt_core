#pragma once

#include <cstdint>
#include <utility>

namespace eerie_leap::domain::system_domain::models {

/// What a unit can do, as a 16-bit mask. The mask is the one description of a unit: the SMP device
/// group, the BLE advertisement and CDMP (heartbeat and discovery response) all carry it.
enum class ProductFeature : uint16_t {
    ANALOG_INPUTS = 1 << 0,
    DIGITAL_INPUTS = 1 << 1,
    CANBUS = 1 << 2,
    LOGGING = 1 << 3,
    BLUETOOTH = 1 << 4,
    DISPLAY = 1 << 5,
    STORAGE = 1 << 6
};

constexpr uint16_t ToMask(ProductFeature feature) {
    return std::to_underlying(feature);
}

constexpr bool HasFeature(uint16_t features, ProductFeature feature) {
    return (features & ToMask(feature)) != 0;
}

namespace detail {

constexpr uint16_t GetBuildFeatures() {
    uint16_t features = 0;

    #if CONFIG_EERIE_LEAP_FEATURES_ANALOG_INPUTS
        features |= ToMask(ProductFeature::ANALOG_INPUTS);
    #endif

    #if CONFIG_EERIE_LEAP_FEATURES_DIGITAL_INPUTS
        features |= ToMask(ProductFeature::DIGITAL_INPUTS);
    #endif

    #if CONFIG_EERIE_LEAP_FEATURES_CANBUS
        features |= ToMask(ProductFeature::CANBUS);
    #endif

    #if CONFIG_EERIE_LEAP_FEATURES_LOGGING
        features |= ToMask(ProductFeature::LOGGING);
    #endif

    #if CONFIG_EERIE_LEAP_FEATURES_BLUETOOTH
        features |= ToMask(ProductFeature::BLUETOOTH);
    #endif

    #if CONFIG_EERIE_LEAP_FEATURES_DISPLAY
        features |= ToMask(ProductFeature::DISPLAY);
    #endif

    #if CONFIG_EERIE_LEAP_FEATURES_STORAGE
        features |= ToMask(ProductFeature::STORAGE);
    #endif

    return features;
}

} // namespace detail

/// The build's features: every EERIE_LEAP_FEATURES_* that is set. LOGGING here means "may log";
/// what a unit sends on CDMP clears it while no log writer is available (CanbusComService).
inline constexpr uint16_t BUILD_FEATURES = detail::GetBuildFeatures();

} // namespace eerie_leap::domain::system_domain::models
