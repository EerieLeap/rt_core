#pragma once

#include <cstdint>

namespace eerie_leap::domain::system_domain::models {

enum class ProductFamily : uint8_t {
    NONE = 0,
    PROCESSING_MODULE = 1,
    DISPLAY_MODULE = 2,
    INPUT_MODULE = 3
};

enum class ProductFeature : uint16_t {
    ANALOG_INPUTS = 1 << 0,
    DIGITAL_INPUTS = 1 << 1,
    CANBUS = 1 << 2,
    LOGGING = 1 << 3,
    BLUETOOTH = 1 << 4,
    DISPLAY = 1 << 5
};

struct ProductInfo {
private:
    static constexpr ProductFamily GetProductFamily() {
        #if CONFIG_EERIE_LEAP_PRODUCT_FAMILY_NONE
            return ProductFamily::NONE;
        #elif CONFIG_EERIE_LEAP_PRODUCT_FAMILY_PROCESSING_MODULE
            return ProductFamily::PROCESSING_MODULE;
        #elif CONFIG_EERIE_LEAP_PRODUCT_FAMILY_DISPLAY_MODULE
            return ProductFamily::DISPLAY_MODULE;
        #elif CONFIG_EERIE_LEAP_PRODUCT_FAMILY_INPUT_MODULE
            return ProductFamily::INPUT_MODULE;
        #else
            return ProductFamily::NONE;
        #endif
    }

    /**
     * @brief Get the features bitmap
     *
     * @return constexpr uint16_t
     */
    static constexpr uint16_t GetFeatures() {
        uint16_t features = 0;

        #if CONFIG_EERIE_LEAP_FEATURES_ANALOG_INPUTS
            features |= static_cast<uint16_t>(ProductFeature::ANALOG_INPUTS);
        #endif

        #if CONFIG_EERIE_LEAP_FEATURES_DIGITAL_INPUTS
            features |= static_cast<uint16_t>(ProductFeature::DIGITAL_INPUTS);
        #endif

        #if CONFIG_EERIE_LEAP_FEATURES_CANBUS
            features |= static_cast<uint16_t>(ProductFeature::CANBUS);
        #endif

        #if CONFIG_EERIE_LEAP_FEATURES_LOGGING
            features |= static_cast<uint16_t>(ProductFeature::LOGGING);
        #endif

        #if CONFIG_EERIE_LEAP_FEATURES_BLUETOOTH
            features |= static_cast<uint16_t>(ProductFeature::BLUETOOTH);
        #endif

        #if CONFIG_EERIE_LEAP_FEATURES_DISPLAY
            features |= static_cast<uint16_t>(ProductFeature::DISPLAY);
        #endif

        return features;
    }

public:
    static const ProductFamily family;
    static const uint16_t product_id;
    static const uint16_t features;
    static const uint8_t revision;
};

inline constexpr ProductFamily ProductInfo::family = ProductInfo::GetProductFamily();
inline constexpr uint16_t ProductInfo::product_id = CONFIG_EERIE_LEAP_PRODUCT_ID;
inline constexpr uint16_t ProductInfo::features = ProductInfo::GetFeatures();
inline constexpr uint8_t ProductInfo::revision = CONFIG_EERIE_LEAP_REVISION;

} // namespace eerie_leap::domain::system_domain::models
