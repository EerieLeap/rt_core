#pragma once

#include <cstdint>

#include "subsys/cdmp/models/cdmp_device.h"

namespace eerie_leap::subsys::cdmp::models {

/** @brief A copy of what the network knows about a device, safe to hand to other threads. */
struct CdmpDeviceInfo {
    uint8_t device_id = CdmpDevice::DEVICE_ID_UNASSIGNED; ///< Claimed CDMP device ID.
    uint32_t uid = 0;                                     ///< Unique identifier, see CdmpUid.
    uint16_t features = 0;                                ///< Application feature mask, see CdmpDevice::GetFeatures().
    CdmpDeviceStatus status = CdmpDeviceStatus::OFFLINE;
};

} // namespace eerie_leap::subsys::cdmp::models
