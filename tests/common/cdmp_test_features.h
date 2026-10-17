#pragma once

#include <cstdint>

#include "domain/system_domain/models/product_feature.h"

namespace eerie_leap::tests {

/// What a log writer and a display unit report on CDMP in the tests.
inline constexpr uint16_t LOGGER_FEATURES =
    eerie_leap::domain::system_domain::models::ToMask(eerie_leap::domain::system_domain::models::ProductFeature::LOGGING);
inline constexpr uint16_t DISPLAY_FEATURES =
    eerie_leap::domain::system_domain::models::ToMask(eerie_leap::domain::system_domain::models::ProductFeature::DISPLAY);

} // namespace eerie_leap::tests
