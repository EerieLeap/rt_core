#pragma once

#include <cstdint>
#include <optional>

#include "subsys/cdmp/utilities/cdmp_can_id_manager.h"

namespace eerie_leap::domain::canbus_domain::models {

using eerie_leap::subsys::cdmp::utilities::CdmpCanIdManager;

// Settings of the channel that carries CDMP and SMP management traffic.
struct CanbusComConfiguration {
    // Bits of the SMP CAN ID that carry the target and source device IDs.
    static constexpr uint32_t SMP_ADDRESS_MASK = 0x0000FFFF;

    std::optional<uint8_t> bus_channel = std::nullopt;
    uint32_t cdmp_base_can_id = CdmpCanIdManager::DEFAULT_BASE_CAN_ID;
    // 29-bit; its top 11 bits rank below vehicle, CDMP and OBD-II traffic.
    uint32_t smp_can_id_base = 0x1FF00000;
    uint8_t smp_bus_share_percent = 25;
};

} // namespace eerie_leap::domain::canbus_domain::models
