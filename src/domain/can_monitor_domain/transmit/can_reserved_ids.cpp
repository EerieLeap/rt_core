#include "domain/can_monitor_domain/transmit/can_reserved_ids.h"

#include <algorithm>

#include <zephyr/drivers/can.h>

namespace eerie_leap::domain::can_monitor_domain::transmit {

using eerie_leap::domain::canbus_domain::models::CanbusComConfiguration;

CanReservedIds::CanReservedIds(uint8_t port, const CanbusComConfiguration& com) {
    if(!com.bus_channel.has_value() || *com.bus_channel != port)
        return;

    // A validated base leaves room for +99; the clamp keeps a configuration that skipped validation inside
    // the 11-bit space, where the range would otherwise name IDs that do not exist.
    const uint32_t cdmp_last = std::min<uint32_t>(com.cdmp_base_can_id + CDMP_LAST_OFFSET, CAN_STD_ID_MASK);
    ranges_[count_++] = { com.cdmp_base_can_id, cdmp_last, false };

    const uint32_t smp_last = (com.smp_can_id_base | SMP_ADDRESS_MASK) & CAN_EXT_ID_MASK;
    ranges_[count_++] = { com.smp_can_id_base, smp_last, true };
}

bool CanReservedIds::IsReserved(uint32_t id, bool ext) const {
    return std::ranges::any_of(Ranges(), [&](const CanReservedRange& range) {
        return range.Contains(id, ext);
    });
}

} // namespace eerie_leap::domain::can_monitor_domain::transmit
