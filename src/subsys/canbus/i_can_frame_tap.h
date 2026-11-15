#pragma once

#include <cstdint>

#include <zephyr/drivers/can.h>

#include "can_frame.h"

namespace eerie_leap::subsys::canbus {

/**
 * @brief Sees everything on one bus: all frames the bus receives, the unit's own, and the bus's state.
 *
 * Set with Canbus::SetTap(). Every method runs on the bus's receive thread, one call at a time, under
 * the bus's lock: it must not block, wait for a lock another thread holds while it calls the bus, or
 * call SetTap(). The thread is cooperative at a very high priority, so keep the work short.
 */
class ICanFrameTap {
public:
    virtual ~ICanFrameTap() = default;

    /// Every frame the bus receives, and every frame the unit sent (is_transmit), on the receive thread.
    virtual void OnFrame(const CanFrame& frame, uint64_t time_us, bool is_transmit, int tx_error) = 0;

    /// A change of the bus's state, on the receive thread.
    virtual void OnState(can_state state, can_bus_err_cnt counters, uint64_t time_us) = 0;

    /// The filter swap SetTap() made has ended; frames around it may have repeated, or (may_have_missed) gone missing.
    /// On the receive thread, before the first entry it handles after the swap.
    virtual void OnFilterSwap(bool may_have_missed, uint64_t time_us) = 0;
};

} // namespace eerie_leap::subsys::canbus
