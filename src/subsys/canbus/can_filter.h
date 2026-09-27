#pragma once

#include <cstdint>

#include "can_id.h"

namespace eerie_leap::subsys::canbus {

// Accepts identifiers of one format whose bits under the mask equal the filter's.
struct CanFilter {
    uint32_t id = 0;
    uint32_t mask = 0;
    bool is_extended = false;

    constexpr CanFilter() = default;

    constexpr CanFilter(uint32_t filter_id, uint32_t filter_mask, bool extended)
        : id(filter_id), mask(filter_mask), is_extended(extended) {}

    // An identifier is the filter that accepts exactly that identifier.
    constexpr CanFilter(const CanId& can_id) // NOLINT(google-explicit-constructor)
        : id(can_id.id), mask(can_id.WidthMask()), is_extended(can_id.is_extended) {}

    static constexpr CanFilter Masked(const CanId& can_id, uint32_t mask) {
        return {can_id.id, mask, can_id.is_extended};
    }

    [[nodiscard]] constexpr uint32_t WidthMask() const { return CanId::WidthMask(is_extended); }

    [[nodiscard]] constexpr bool IsValid() const {
        return (id & ~WidthMask()) == 0 && (mask & ~WidthMask()) == 0;
    }

    [[nodiscard]] constexpr bool IsExact() const { return mask == WidthMask(); }

    // Bits outside the mask never take part in matching, so equal filters compare equal.
    [[nodiscard]] constexpr CanFilter Normalized() const { return {id & mask, mask, is_extended}; }

    [[nodiscard]] constexpr bool Matches(const CanId& can_id) const {
        return can_id.is_extended == is_extended && ((can_id.id ^ id) & mask) == 0;
    }

    // True when at least one identifier passes both filters.
    [[nodiscard]] constexpr bool Overlaps(const CanFilter& other) const {
        return other.is_extended == is_extended && ((other.id ^ id) & mask & other.mask) == 0;
    }

    constexpr bool operator==(const CanFilter&) const = default;
};

} // namespace eerie_leap::subsys::canbus
