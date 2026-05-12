#include <ctime>
#include <zephyr/kernel.h>

#include "boot_elapsed_time_provider.h"

namespace eerie_leap::subsys::time {

time_point BootElapsedTimeProvider::GetTime() {
	return time_point(std::chrono::milliseconds(k_uptime_get()));
}

} // namespace eerie_leap::subsys::time
