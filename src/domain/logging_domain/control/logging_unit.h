#pragma once

#include "subsys/cdmp/models/cdmp_device_info.h"

namespace eerie_leap::domain::logging_domain::control {

// Whether a unit in the CDMP device table can take a LOGGING start. The one place that says so.
[[nodiscard]] inline bool CanLog(const eerie_leap::subsys::cdmp::models::CdmpDeviceInfo& device) {
    return device.device_type == eerie_leap::subsys::cdmp::models::CdmpDeviceType::LOGGER;
}

} // namespace eerie_leap::domain::logging_domain::control
