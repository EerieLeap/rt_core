#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "domain/canbus_domain/models/canbus_com_configuration.h"

namespace eerie_leap::domain::can_monitor_domain::transmit {

/** @brief The identifiers from @c from to @c to, both included, of one format. */
struct CanReservedRange {
    uint32_t from = 0;
    uint32_t to = 0;
    /** @brief A 29-bit range (otherwise an 11-bit one). */
    bool ext = false;

    [[nodiscard]] constexpr bool Contains(uint32_t id, bool is_extended) const {
        return is_extended == ext && id >= from && id <= to;
    }

    constexpr bool operator==(const CanReservedRange&) const = default;
};

/**
 * @brief The identifiers a session may not transmit: the network's own on the COM channel.
 *
 * Only the port that is the stored configuration's @c bus_channel has any (R-12): the CDMP IDs, the
 * 11-bit @c cdmp_base_can_id to +99, and the SMP IDs, the 29-bit @c smp_can_id_base to
 * @c smp_can_id_base | 0xFFFF. Every other port, and a configuration without a COM channel, reserves
 * nothing. The ranges are a snapshot: a new configuration needs a new object (and disarms the session).
 */
class CanReservedIds {
private:
    std::array<CanReservedRange, 2> ranges_{};
    size_t count_ = 0;

public:
    /** @brief The last CDMP offset: Base+0 to Base+99. */
    static constexpr uint32_t CDMP_LAST_OFFSET = 99;
    /** @brief The SMP IDs vary in their low 16 bits (target and source). */
    static constexpr uint32_t SMP_ADDRESS_MASK = 0xFFFF;

    /** @brief Nothing reserved. */
    CanReservedIds() = default;

    /**
     * @param port The session's port (the channel index).
     * @param com The stored COM configuration.
     */
    CanReservedIds(uint8_t port, const eerie_leap::domain::canbus_domain::models::CanbusComConfiguration& com);

    /** @brief The reserved ranges, the 11-bit one first; empty when nothing is. */
    [[nodiscard]] std::span<const CanReservedRange> Ranges() const {
        return { ranges_.data(), count_ };
    }

    [[nodiscard]] bool Empty() const {
        return count_ == 0;
    }

    /** @brief Whether the identifier, in the format @p ext, is in a reserved range. */
    [[nodiscard]] bool IsReserved(uint32_t id, bool ext) const;
};

} // namespace eerie_leap::domain::can_monitor_domain::transmit
