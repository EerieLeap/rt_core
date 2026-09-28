#pragma once

#include <cstdint>

#include "can_id.h"

namespace eerie_leap::subsys::canbus {

/** @brief Accepts identifiers of one format whose bits under the mask equal the filter's. */
struct CanFilter {
    uint32_t id = 0;          ///< Identifier bits to match.
    uint32_t mask = 0;        ///< Bits of the identifier that take part in matching.
    bool is_extended = false; ///< Format of the accepted identifiers.

    constexpr CanFilter() = default;

    constexpr CanFilter(uint32_t filter_id, uint32_t filter_mask, bool extended)
        : id(filter_id), mask(filter_mask), is_extended(extended) {}

    /** @brief Creates the filter that accepts exactly @p can_id. */
    constexpr CanFilter(const CanId& can_id) // NOLINT(google-explicit-constructor)
        : id(can_id.id), mask(can_id.WidthMask()), is_extended(can_id.is_extended) {}

    /** @brief Creates a filter for the format of @p can_id that compares only the bits in @p mask. */
    static constexpr CanFilter Masked(const CanId& can_id, uint32_t mask) {
        return {can_id.id, mask, can_id.is_extended};
    }

    /** @brief Returns the mask of all identifier bits of this format. */
    [[nodiscard]] constexpr uint32_t WidthMask() const { return CanId::WidthMask(is_extended); }

    /** @brief True when the identifier and the mask fit the format. */
    [[nodiscard]] constexpr bool IsValid() const {
        return (id & ~WidthMask()) == 0 && (mask & ~WidthMask()) == 0;
    }

    /** @brief True when the filter accepts a single identifier. */
    [[nodiscard]] constexpr bool IsExact() const { return mask == WidthMask(); }

    /** @brief Clears the bits outside the mask, so equal filters compare equal. */
    [[nodiscard]] constexpr CanFilter Normalized() const { return {id & mask, mask, is_extended}; }

    /** @brief True when the filter accepts @p can_id. */
    [[nodiscard]] constexpr bool Matches(const CanId& can_id) const {
        return can_id.is_extended == is_extended && ((can_id.id ^ id) & mask) == 0;
    }

    /** @brief True when at least one identifier passes both filters. */
    [[nodiscard]] constexpr bool Overlaps(const CanFilter& other) const {
        return other.is_extended == is_extended && ((other.id ^ id) & mask & other.mask) == 0;
    }

    constexpr bool operator==(const CanFilter&) const = default;
};

} // namespace eerie_leap::subsys::canbus
