#include <algorithm>
#include <array>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>

#include "subsys/random/rng.h"

#include "cdmp_uid.h"

LOG_MODULE_REGISTER(cdmp_uid, LOG_LEVEL_INF);

namespace eerie_leap::subsys::cdmp::utilities {

using eerie_leap::subsys::random::Rng;

uint32_t CdmpUid::Generate() {
    std::array<uint8_t, 16> hardware_id{};
    const ssize_t length = hwinfo_get_device_id(hardware_id.data(), hardware_id.size());

    if(length > 0) {
        auto uid = FromHardwareId(std::span<const uint8_t>(hardware_id.data(), static_cast<size_t>(length)));
        if(uid.has_value())
            return uid.value();
    }

    LOG_WRN("No hardware id available (%d), using a random CDMP UID.", static_cast<int>(length));

    uint32_t uid = 0;
    while(uid == 0)
        uid = Rng::Get<uint32_t>(true);

    return uid;
}

std::optional<uint32_t> CdmpUid::FromHardwareId(std::span<const uint8_t> hardware_id) {
    if(hardware_id.empty())
        return std::nullopt;

    const bool is_blank = std::ranges::all_of(hardware_id, [](uint8_t b) { return b == 0x00; })
        || std::ranges::all_of(hardware_id, [](uint8_t b) { return b == 0xFF; });
    if(is_blank)
        return std::nullopt;

    // 0 is reserved for "no UID".
    const uint32_t uid = crc32_ieee(hardware_id.data(), hardware_id.size());
    return uid != 0 ? uid : 1U;
}

} // namespace eerie_leap::subsys::cdmp::utilities
