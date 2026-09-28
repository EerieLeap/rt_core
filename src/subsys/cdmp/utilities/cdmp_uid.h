#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace eerie_leap::subsys::cdmp::utilities {

/** @brief Derives the 32-bit CDMP unique identifier. */
class CdmpUid {
public:
    /**
     * @brief Returns the UID of this unit.
     *
     * Stable across reboots when the SoC exposes a hardware ID (eFuse MAC on ESP32), random and
     * non-zero otherwise.
     */
    static uint32_t Generate();

    /**
     * @brief Folds a hardware ID into a non-zero UID with CRC32.
     * @return std::nullopt for an empty, all-zero or all-ones (unprogrammed) ID.
     */
    static std::optional<uint32_t> FromHardwareId(std::span<const uint8_t> hardware_id);
};

} // namespace eerie_leap::subsys::cdmp::utilities
