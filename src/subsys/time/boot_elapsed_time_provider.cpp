#include <ctime>
#include <zephyr/kernel.h>

#include "boot_elapsed_time_provider.h"

namespace eerie_leap::subsys::time {

std::chrono::system_clock::time_point BootElapsedTimeProvider::GetTime() {
	return std::chrono::system_clock::time_point(std::chrono::milliseconds(k_uptime_get()));
}

} // namespace eerie_leap::subsys::time
