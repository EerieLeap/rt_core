#include <algorithm>

#include "gpio_random_signal.h"

namespace eerie_leap::subsys::gpio {

GpioRandomSignalConfig GpioRandomSignal::Normalize(GpioRandomSignalConfig config) {
    // A zero hold would flip forever at one instant.
    config.hold_min_ms = std::max<uint32_t>(config.hold_min_ms, 1);
    config.hold_max_ms = std::max(config.hold_min_ms, config.hold_max_ms);

    return config;
}

GpioRandomSignal::GpioRandomSignal(const GpioRandomSignalConfig& config) :
    config_(Normalize(config)),
    rng_(config.seed),
    level_((rng_.Next() & 1U) != 0) {}

int64_t GpioRandomSignal::NextHold() {
    return rng_.NextInRange(config_.hold_min_ms, config_.hold_max_ms);
}

bool GpioRandomSignal::At(int64_t now_ms) {
    if(!started_) {
        started_ = true;
        next_flip_ms_ = now_ms + NextHold();
    }

    int flips = 0;
    while(now_ms >= next_flip_ms_) {
        level_ = !level_;

        if(++flips > MAX_CATCH_UP_FLIPS) {
            next_flip_ms_ = now_ms + NextHold();

            break;
        }

        next_flip_ms_ += NextHold();
    }

    return level_;
}

int64_t GpioRandomSignal::GetNextFlipMs() const {
    return next_flip_ms_;
}

} // namespace eerie_leap::subsys::gpio
