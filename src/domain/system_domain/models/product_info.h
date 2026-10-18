#pragma once

#include <cstdint>

#include "domain/system_domain/models/product_feature.h"

namespace eerie_leap::domain::system_domain::models {

struct ProductInfo {
    static const uint16_t PRODUCT_ID;
    /// The build's features, see BUILD_FEATURES.
    static const uint16_t FEATURES;
    static const uint8_t REVISION;
};

inline constexpr uint16_t ProductInfo::PRODUCT_ID = CONFIG_EERIE_LEAP_PRODUCT_ID;
inline constexpr uint16_t ProductInfo::FEATURES = BUILD_FEATURES;
inline constexpr uint8_t ProductInfo::REVISION = CONFIG_EERIE_LEAP_REVISION;

} // namespace eerie_leap::domain::system_domain::models
