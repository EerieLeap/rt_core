#pragma once

#include <cstdint>
#include <optional>

#include "subsys/cdmp/utilities/cdmp_can_id_manager.h"
#include "subsys/smp/can/smp_can_id_layout.h"

namespace eerie_leap::domain::canbus_domain::models {

using eerie_leap::subsys::cdmp::utilities::CdmpCanIdManager;
using eerie_leap::subsys::smp::can::SmpCanIdLayout;

/** @brief Settings of the channel that carries CDMP and SMP management traffic. */
struct CanbusComConfiguration {
    std::optional<uint8_t> bus_channel = std::nullopt; ///< Channel with CDMP and SMP; none disables them.
    uint32_t cdmp_base_can_id = CdmpCanIdManager::DEFAULT_BASE_CAN_ID; ///< 11-bit base of the CDMP IDs.
    /// 29-bit base of the SMP IDs; its top 11 bits rank below vehicle, CDMP and OBD-II traffic.
    uint32_t smp_can_id_base = SmpCanIdLayout::DEFAULT_BASE;
    uint8_t smp_bus_share_percent = 25; ///< Cap on the bus load of this unit's SMP frames, 1-100.
};

} // namespace eerie_leap::domain::canbus_domain::models
