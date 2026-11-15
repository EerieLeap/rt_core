#pragma once

#include <cstdint>

#include <zephyr/kernel.h>
#include <zephyr/sys/time_units.h>

namespace eerie_leap::subsys::canbus {

/**
 * @brief Microseconds since boot: the base of every time the CAN subsystem stamps on a frame or an event.
 *
 * Read from the 64-bit cycle counter where the platform has one, which resolves frames a few hundred
 * microseconds apart. Without one it falls back to the kernel tick, whose length is the resolution
 * (100 µs to 1 ms on the targets that matter). Safe in any context.
 */
inline uint64_t NowUs() {
#ifdef CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER
    return k_cyc_to_us_floor64(k_cycle_get_64());
#else
    return k_ticks_to_us_floor64(k_uptime_ticks());
#endif
}

} // namespace eerie_leap::subsys::canbus
