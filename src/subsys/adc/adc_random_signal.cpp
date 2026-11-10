#include <algorithm>

#include "adc_random_signal.h"

namespace eerie_leap::subsys::adc {

AdcRandomSignalConfig AdcRandomSignal::Normalize(AdcRandomSignalConfig config) {
    config.volts_max = std::max(config.volts_max, 0.0F);
    config.move_max_ms = std::max(config.move_min_ms, config.move_max_ms);

    return config;
}

AdcRandomSignal::AdcRandomSignal(const AdcRandomSignalConfig& config) :
    config_(Normalize(config)),
    rng_(config.seed),
    to_volts_(rng_.NextFloat() * config_.volts_max),
    from_volts_(to_volts_) {}

void AdcRandomSignal::StartSegment(int64_t start_ms) {
    from_volts_ = to_volts_;
    to_volts_ = rng_.NextFloat() * config_.volts_max;

    const uint32_t move_ms = rng_.NextInRange(config_.move_min_ms, config_.move_max_ms);
    const uint32_t hold_ms = rng_.NextInRange(0, config_.hold_max_ms);

    move_start_ms_ = start_ms;
    move_end_ms_ = start_ms + move_ms;
    segment_end_ms_ = move_end_ms_ + hold_ms;
}

float AdcRandomSignal::At(int64_t now_ms) {
    if(!started_) {
        started_ = true;
        StartSegment(now_ms);
    }

    int segments = 0;
    while(now_ms >= segment_end_ms_) {
        if(++segments > MAX_CATCH_UP_SEGMENTS) {
            StartSegment(now_ms);

            break;
        }

        StartSegment(segment_end_ms_);
    }

    if(now_ms <= move_start_ms_)
        return from_volts_;

    if(now_ms >= move_end_ms_)
        return to_volts_;

    const float t = static_cast<float>(now_ms - move_start_ms_) / static_cast<float>(move_end_ms_ - move_start_ms_);
    const float eased = t * t * (3.0F - 2.0F * t);

    return from_volts_ + (to_volts_ - from_volts_) * eased;
}

} // namespace eerie_leap::subsys::adc
