#pragma once

#include <cstdint>

#include "subsys/cdmp/models/cdmp_device.h"

namespace eerie_leap::subsys::cdmp::models {

// A copy of what the network knows about a device, safe to hand to other threads.
struct CdmpDeviceInfo {
    uint8_t device_id = CdmpDevice::DEVICE_ID_UNASSIGNED;
    uint32_t uid = 0;
    CdmpDeviceType device_type = CdmpDeviceType::NONE;
    CdmpDeviceStatus status = CdmpDeviceStatus::OFFLINE;
};

} // namespace eerie_leap::subsys::cdmp::models
