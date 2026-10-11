#pragma once

#include <chrono>

namespace eerie_leap::domain::sensor_domain::sensor_readers {

// What every reader of one rate group tick shares: taken once, after the group's ADC samples
// were acquired, so all readings of the tick carry the same time.
struct TickContext {
    std::chrono::system_clock::time_point timestamp;
};

} // namespace eerie_leap::domain::sensor_domain::sensor_readers
