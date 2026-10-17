#pragma once

#include <cstdint>

#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>

namespace eerie_leap::subsys::smp {

/** @brief Application SMP groups, starting at MCUmgr's first user group ID. */
enum class SmpGroupId : uint16_t {
    CONFIG = MGMT_GROUP_ID_PERUSER,
    DEVICE = MGMT_GROUP_ID_PERUSER + 1,
    NETWORK = MGMT_GROUP_ID_PERUSER + 2,
    LIVE = MGMT_GROUP_ID_PERUSER + 3,
    CALIBRATION = MGMT_GROUP_ID_PERUSER + 4,
    FILES = MGMT_GROUP_ID_PERUSER + 5,
    ASSETS = MGMT_GROUP_ID_PERUSER + 6,
};

} // namespace eerie_leap::subsys::smp
