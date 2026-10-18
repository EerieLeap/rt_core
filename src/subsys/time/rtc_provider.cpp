#include <ctime>
#include <zephyr/kernel.h>

#include "rtc_provider.h"

namespace eerie_leap::subsys::time {

std::chrono::system_clock::time_point RtcProvider::GetTime() {
    uint64_t fake_start_time = 1761106217000;
    // Uptime at the kernel tick's resolution (100 us at 10 kHz), not truncated to milliseconds.
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(fake_start_time))
        + std::chrono::microseconds(k_ticks_to_us_floor64(k_uptime_ticks()));
}

} // namespace eerie_leap::subsys::time
