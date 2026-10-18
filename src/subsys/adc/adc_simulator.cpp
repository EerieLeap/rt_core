#include <algorithm>
#include <cerrno>

#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

#include "adc_simulator.h"

namespace eerie_leap::subsys::adc {

LOG_MODULE_REGISTER(adc_simulator_logger);

bool AdcSimulator::Initialize() {
    LOG_INF("Adc Simulator initialization started.");

    LOG_INF("Adc Simulator initialized successfully.");

    return true;
}

void AdcSimulator::Sample(const AdcSampleRequest& request) noexcept {
    const size_t count = std::min(request.channels.size(), request.results.size());

    stats_.requests++;

    // The same request contract as Adc: channels in range and ascending.
    int last_channel = -1;
    for(size_t i = 0; i < count; i++) {
        request.results[i] = {};

        const int channel = request.channels[i];
        if(channel >= CHANNEL_COUNT || channel <= last_channel) {
            request.results[i].error = AdcError::CHANNEL_OUT_OF_RANGE;
            request.results[i].errno_value = -EINVAL;
            stats_.errors++;
            continue;
        }

        last_channel = channel;

        const uint32_t raw = sys_rand32_get();
        request.results[i].volts = (raw / static_cast<float>(UINT32_MAX)) * 3.3F;
        stats_.conversions++;
    }
}

int AdcSimulator::GetChannelCount() const {
    return CHANNEL_COUNT;
}

AdcCapabilities AdcSimulator::GetCapabilities() const {
    return { .burst = false, .multi_channel = false, .us_per_conversion = 0 };
}

AdcStats AdcSimulator::GetStats() const {
    return stats_;
}

} // namespace eerie_leap::subsys::adc
