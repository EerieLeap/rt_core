#pragma once

#include <cstdint>

#include <zephyr/drivers/can.h>

namespace eerie_leap::subsys::canbus {

/**
 * @brief A CAN identifier together with its format.
 *
 * Standard 0x123 and extended 0x123 are different identifiers on the wire.
 */
struct CanId {
    uint32_t id = 0;          ///< Identifier bits, 11 or 29 wide.
    bool is_extended = false; ///< True for a 29-bit (CAN 2.0B) identifier.

    /** @brief Creates an 11-bit identifier. */
    static constexpr CanId Standard(uint32_t id) { return {id, false}; }
    /** @brief Creates a 29-bit identifier. */
    static constexpr CanId Extended(uint32_t id) { return {id, true}; }

    /** @brief Returns the mask of all identifier bits of the given format. */
    static constexpr uint32_t WidthMask(bool is_extended) {
        return is_extended ? CAN_EXT_ID_MASK : CAN_STD_ID_MASK;
    }

    /** @brief Returns the mask of all identifier bits of this format. */
    [[nodiscard]] constexpr uint32_t WidthMask() const { return WidthMask(is_extended); }
    /** @brief True when the identifier fits its format. */
    [[nodiscard]] constexpr bool IsValid() const { return (id & ~WidthMask()) == 0; }

    /** @brief Returns a key unique across both formats; bit 31 is never part of a CAN identifier. */
    [[nodiscard]] constexpr uint32_t Key() const { return id | (is_extended ? 0x80000000U : 0U); }

    constexpr bool operator==(const CanId&) const = default;
};

} // namespace eerie_leap::subsys::canbus
