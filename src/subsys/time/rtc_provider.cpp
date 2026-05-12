#include <ctime>
#include <zephyr/kernel.h>

#include "rtc_provider.h"

namespace eerie_leap::subsys::time {

time_point RtcProvider::GetTime() {
	uint64_t fake_start_time = 1761106217000;
	return time_point(std::chrono::milliseconds(fake_start_time + k_uptime_get()));
}

} // namespace eerie_leap::subsys::time
