#include "can_error_counters.h"

#include <zephyr/drivers/can.h>

namespace eerie_leap::domain::can_monitor_domain::services {

CanErrorCounts DeviceCanErrorCounters::Read() const {
    CanErrorCounts counts;

#ifdef CONFIG_CAN_STATS
    if(device_ != nullptr) {
        counts.errors = { can_stats_get_bit_errors(device_), can_stats_get_stuff_errors(device_),
            can_stats_get_crc_errors(device_), can_stats_get_form_errors(device_), can_stats_get_ack_errors(device_) };
        counts.rx_overruns = can_stats_get_rx_overruns(device_);
    }
#else
    (void)device_;
#endif

    return counts;
}

} // namespace eerie_leap::domain::can_monitor_domain::services
