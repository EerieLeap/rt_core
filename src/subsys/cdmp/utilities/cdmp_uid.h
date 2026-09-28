#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace eerie_leap::subsys::cdmp::utilities {

class CdmpUid {
public:
    // Stable across reboots when the SoC exposes a hardware id, random otherwise.
    static uint32_t Generate();

    // Folds a hardware id into a non-zero UID; std::nullopt for an empty or unprogrammed id.
    static std::optional<uint32_t> FromHardwareId(std::span<const uint8_t> hardware_id);
};

} // namespace eerie_leap::subsys::cdmp::utilities
