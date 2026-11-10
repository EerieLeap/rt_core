#pragma once

#include <cstdint>

#include "utilities/math/xorshift32.hpp"

namespace eerie_leap::subsys::adc {

struct AdcRandomSignalConfig {
    float volts_max = 3.3F;
    uint32_t move_min_ms = 200;
    uint32_t move_max_ms = 4000;
    uint32_t hold_max_ms = 2000;
    uint32_t seed = 1;
};

// A voltage that wanders: it eases (smoothstep) from where it is to a random target in
// [0, volts_max] over a random time, holds for a random time, and repeats. Pure: the caller
// passes the time, so a test can drive it without a clock.
class AdcRandomSignal {
private:
    // A call after this many segments' worth of time restarts at `now` instead of replaying the
    // gap, so a signal that was not read for hours costs the same as one read every tick.
    static constexpr int MAX_CATCH_UP_SEGMENTS = 16;

    AdcRandomSignalConfig config_;
    eerie_leap::utilities::math::Xorshift32 rng_;

    float to_volts_;
    float from_volts_;
    int64_t move_start_ms_ = 0;
    int64_t move_end_ms_ = 0;
    int64_t segment_end_ms_ = 0;
    bool started_ = false;

    static AdcRandomSignalConfig Normalize(AdcRandomSignalConfig config);
    void StartSegment(int64_t start_ms);

public:
    explicit AdcRandomSignal(const AdcRandomSignalConfig& config);

    // Volts at `now_ms`. Time is expected to grow; an earlier time returns the segment's start.
    float At(int64_t now_ms);
};

} // namespace eerie_leap::subsys::adc
