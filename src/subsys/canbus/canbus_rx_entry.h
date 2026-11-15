#pragma once

#include <algorithm>
#include <cstdint>
#include <type_traits>

#include <zephyr/drivers/can.h>

namespace eerie_leap::subsys::canbus {

enum class CanbusRxKind : uint8_t {
    RECEIVED,     ///< A frame the controller received.
    TX_ECHO,      ///< A frame the unit sent, queued by its completion.
    STATE_CHANGE, ///< The controller changed state.
};

/**
 * @brief What the driver contexts hand to the receive thread through its queue.
 *
 * Trivially copyable, because k_msgq copies it byte for byte. Build one with the named constructors;
 * the fields a kind does not use stay zero.
 */
struct CanbusRxEntry {
    can_frame frame{};    ///< RECEIVED and TX_ECHO.
    uint64_t time_us = 0; ///< CAN subsystem time (NowUs()), taken in the driver's context.
    CanbusRxKind kind = CanbusRxKind::RECEIVED;
    int8_t tx_error = 0;                      ///< TX_ECHO: the completion's error, clamped to int8.
    can_state state = CAN_STATE_ERROR_ACTIVE; ///< STATE_CHANGE.
    can_bus_err_cnt err_cnt{};                ///< STATE_CHANGE: the TX and RX error counters.

    static constexpr int8_t ClampError(int error) {
        return static_cast<int8_t>(std::clamp<int>(error, INT8_MIN, INT8_MAX));
    }

    static CanbusRxEntry Received(const can_frame& frame, uint64_t time_us) {
        CanbusRxEntry entry;
        entry.frame = frame;
        entry.time_us = time_us;
        entry.kind = CanbusRxKind::RECEIVED;

        return entry;
    }

    static CanbusRxEntry TxEcho(const can_frame& frame, uint64_t time_us, int error) {
        CanbusRxEntry entry;
        entry.frame = frame;
        entry.time_us = time_us;
        entry.kind = CanbusRxKind::TX_ECHO;
        entry.tx_error = ClampError(error);

        return entry;
    }

    static CanbusRxEntry StateChange(can_state state, can_bus_err_cnt err_cnt, uint64_t time_us) {
        CanbusRxEntry entry;
        entry.time_us = time_us;
        entry.kind = CanbusRxKind::STATE_CHANGE;
        entry.state = state;
        entry.err_cnt = err_cnt;

        return entry;
    }
};

static_assert(std::is_trivially_copyable_v<CanbusRxEntry>, "k_msgq copies the entry byte for byte");

} // namespace eerie_leap::subsys::canbus
