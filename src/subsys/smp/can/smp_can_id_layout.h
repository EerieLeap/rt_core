#pragma once

#include <cstdint>
#include <stdexcept>

namespace eerie_leap::subsys::smp::can {

/** @brief SMP-CAN 29-bit identifier: `base | target << 8 | source`. */
class SmpCanIdLayout {
private:
    uint32_t base_ = DEFAULT_BASE;

public:
    static constexpr uint32_t DEFAULT_BASE = 0x1FF00000;
    static constexpr uint32_t ID_MASK = 0x1FFFFFFF;
    static constexpr uint32_t ADDRESS_MASK = 0x0000FFFF; ///< Target and source bits.
    /// Filter mask that matches the base and the target, from any source.
    static constexpr uint32_t TARGET_FILTER_MASK = ID_MASK & ~0xFFU;

    /** @brief True for a 29-bit base with the address bits clear. */
    static constexpr bool IsValidBase(uint32_t base) {
        return (base & ~ID_MASK) == 0 && (base & ADDRESS_MASK) == 0;
    }

    /** @brief True for 1-254; 0 (unassigned) and 0xFF (broadcast) are CDMP IDs that SMP never uses. */
    static constexpr bool IsValidAddress(uint8_t address) {
        return address != 0x00 && address != 0xFF;
    }

    constexpr SmpCanIdLayout() = default;

    /** @throws std::invalid_argument if the base fails IsValidBase(). */
    constexpr explicit SmpCanIdLayout(uint32_t base) : base_(base) {
        if(!IsValidBase(base))
            throw std::invalid_argument("SMP CAN ID base must be 29-bit with the address bits clear");
    }

    [[nodiscard]] constexpr uint32_t GetBase() const { return base_; }

    /** @brief Identifier of a frame from @p source to @p target. */
    [[nodiscard]] constexpr uint32_t MakeId(uint8_t target, uint8_t source) const {
        return base_ | (static_cast<uint32_t>(target) << 8) | source;
    }

    /** @brief Filter ID that, with TARGET_FILTER_MASK, accepts every frame to @p target. */
    [[nodiscard]] constexpr uint32_t GetTargetFilterId(uint8_t target) const {
        return MakeId(target, 0);
    }

    /** @brief True when @p id is an SMP identifier of this base. */
    [[nodiscard]] constexpr bool Contains(uint32_t id) const {
        return (id & ID_MASK & ~ADDRESS_MASK) == base_ && (id & ~ID_MASK) == 0;
    }

    /** @brief Target device ID of an SMP identifier. */
    static constexpr uint8_t GetTarget(uint32_t id) { return static_cast<uint8_t>(id >> 8); }
    /** @brief Source device ID of an SMP identifier. */
    static constexpr uint8_t GetSource(uint32_t id) { return static_cast<uint8_t>(id); }
};

} // namespace eerie_leap::subsys::smp::can
