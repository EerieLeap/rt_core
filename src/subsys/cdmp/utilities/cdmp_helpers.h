#pragma once

#include <cstdint>
#include <span>
#include <set>

#include "subsys/cdmp/utilities/constants.h"
#include "subsys/cdmp/utilities/enums.h"

namespace eerie_leap::subsys::cdmp::utilities {

class CdmpHelpers {
public:
    static uint32_t CalculateStaggeredMessageTimeOffset(std::span<const uint8_t> device_ids, uint8_t device_id) {
        std::set<uint8_t> sorted_device_ids(device_ids.begin(), device_ids.end());
        sorted_device_ids.insert(device_id);

        return std::distance(sorted_device_ids.begin(), sorted_device_ids.find(device_id))
            * CdmpConstants::STAGGERED_MESSAGE_TIME_OFFSET_MS;
    }
};

} // namespace eerie_leap::subsys::cdmp::utilities
