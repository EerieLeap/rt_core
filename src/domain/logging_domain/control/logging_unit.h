#pragma once

#include "domain/system_domain/models/product_feature.h"
#include "subsys/cdmp/models/cdmp_device_info.h"

namespace eerie_leap::domain::logging_domain::control {

// Whether a unit in the CDMP device table can take a LOGGING start. The one place that says so.
[[nodiscard]] inline bool CanLog(const eerie_leap::subsys::cdmp::models::CdmpDeviceInfo& device) {
    return eerie_leap::domain::system_domain::models::HasFeature(
        device.features, eerie_leap::domain::system_domain::models::ProductFeature::LOGGING);
}

} // namespace eerie_leap::domain::logging_domain::control
