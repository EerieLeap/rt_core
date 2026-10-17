#pragma once

#include <cstdint>

#include "domain/system_domain/models/product_feature.h"

namespace eerie_leap::domain::system_domain::models {

struct ProductInfo {
    static const uint16_t product_id;
    /// The build's features, see BUILD_FEATURES.
    static const uint16_t features;
    static const uint8_t revision;
};

inline constexpr uint16_t ProductInfo::product_id = CONFIG_EERIE_LEAP_PRODUCT_ID;
inline constexpr uint16_t ProductInfo::features = BUILD_FEATURES;
inline constexpr uint8_t ProductInfo::revision = CONFIG_EERIE_LEAP_REVISION;

} // namespace eerie_leap::domain::system_domain::models
