#pragma once

#include <cstdint>

#include <zephyr/drivers/can.h>

namespace eerie_leap::subsys::canbus {

// Standard 0x123 and extended 0x123 are different identifiers on the wire.
struct CanId {
    uint32_t id = 0;
    bool is_extended = false;

    static constexpr CanId Standard(uint32_t id) { return {id, false}; }
    static constexpr CanId Extended(uint32_t id) { return {id, true}; }

    static constexpr uint32_t WidthMask(bool is_extended) {
        return is_extended ? CAN_EXT_ID_MASK : CAN_STD_ID_MASK;
    }

    [[nodiscard]] constexpr uint32_t WidthMask() const { return WidthMask(is_extended); }
    [[nodiscard]] constexpr bool IsValid() const { return (id & ~WidthMask()) == 0; }

    // Unique across both formats; bit 31 is never part of a CAN identifier.
    [[nodiscard]] constexpr uint32_t Key() const { return id | (is_extended ? 0x80000000U : 0U); }

    constexpr bool operator==(const CanId&) const = default;
};

} // namespace eerie_leap::subsys::canbus
