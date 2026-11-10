#include <algorithm>
#include <cerrno>
#include <utility>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

#include "adc_randomizer.h"

namespace eerie_leap::subsys::adc {

LOG_MODULE_REGISTER(adc_randomizer_logger);

AdcRandomizer::AdcRandomizer(const AdcRandomizerConfig& config) {
    const uint32_t base_seed = config.seed != 0 ? config.seed : sys_rand32_get();
    const int channel_count = std::max(config.channel_count, 0);

    signals_.reserve(channel_count);
    for(int i = 0; i < channel_count; i++) {
        // Distinct streams per channel, still determined by the one seed.
        const uint32_t seed = base_seed + (static_cast<uint32_t>(i) + 1U) * 0x9E3779B9U;

        signals_.emplace_back(
            AdcRandomSignalConfig{
                .volts_max = config.volts_max,
                .move_min_ms = config.move_min_ms,
                .move_max_ms = config.move_max_ms,
                .hold_max_ms = config.hold_max_ms,
                .seed = seed,
            }
        );
    }
}

bool AdcRandomizer::Initialize() {
    LOG_INF("Adc Randomizer initialized: %d channels.", GetChannelCount());

    return true;
}

void AdcRandomizer::Sample(const AdcSampleRequest& request) noexcept {
    const size_t count = std::min(request.channels.size(), request.results.size());
    const int64_t now_ms = k_uptime_get();

    const k_spinlock_key_t key = k_spin_lock(&lock_);

    stats_.requests++;

    // The same request contract as Adc: channels in range and ascending.
    int last_channel = -1;
    for(size_t i = 0; i < count; i++) {
        request.results[i] = {};

        const int channel = request.channels[i];
        if(std::cmp_greater_equal(channel, signals_.size()) || channel <= last_channel) {
            request.results[i].error = AdcError::CHANNEL_OUT_OF_RANGE;
            request.results[i].errno_value = -EINVAL;
            stats_.errors++;
            continue;
        }

        last_channel = channel;

        // The signal is a function of time, so the mean of `samples` conversions is the value itself;
        // like the simulator, one conversion is counted per channel.
        request.results[i].volts = signals_[channel].At(now_ms);
        stats_.conversions++;
    }

    k_spin_unlock(&lock_, key);
}

int AdcRandomizer::GetChannelCount() const {
    return static_cast<int>(signals_.size());
}

AdcCapabilities AdcRandomizer::GetCapabilities() const {
    return { .burst = true, .multi_channel = true, .us_per_conversion = 0 };
}

AdcStats AdcRandomizer::GetStats() const {
    const k_spinlock_key_t key = k_spin_lock(&lock_);
    const AdcStats stats = stats_;
    k_spin_unlock(&lock_, key);

    return stats;
}

} // namespace eerie_leap::subsys::adc
