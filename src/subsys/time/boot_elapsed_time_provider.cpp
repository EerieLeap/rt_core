#include <ctime>
#include <zephyr/kernel.h>

#include "boot_elapsed_time_provider.h"

namespace eerie_leap::subsys::time {

std::chrono::system_clock::time_point BootElapsedTimeProvider::GetTime() {
    // The kernel tick's resolution (100 us at 10 kHz), not truncated to milliseconds.
    return std::chrono::system_clock::time_point(std::chrono::microseconds(k_ticks_to_us_floor64(k_uptime_ticks())));
}

} // namespace eerie_leap::subsys::time
