#pragma once

#include <cstdint>

#include "utilities/math/xorshift32.hpp"

namespace eerie_leap::subsys::gpio {

struct GpioRandomSignalConfig {
    uint32_t hold_min_ms = 100;
    uint32_t hold_max_ms = 5000;
    uint32_t seed = 1;
};

// A level that holds for a random time and flips. Pure: the caller passes the time.
class GpioRandomSignal {
private:
    // See AdcRandomSignal: a long gap restarts at `now` instead of replaying every flip.
    static constexpr int MAX_CATCH_UP_FLIPS = 16;

    GpioRandomSignalConfig config_;
    eerie_leap::utilities::math::Xorshift32 rng_;

    bool level_;
    int64_t next_flip_ms_ = 0;
    bool started_ = false;

    static GpioRandomSignalConfig Normalize(GpioRandomSignalConfig config);
    int64_t NextHold();

public:
    explicit GpioRandomSignal(const GpioRandomSignalConfig& config);

    // Level at `now_ms`; the first call starts the signal.
    bool At(int64_t now_ms);

    // When the level flips next, as of the last At(); 0 before the first call.
    [[nodiscard]] int64_t GetNextFlipMs() const;
};

} // namespace eerie_leap::subsys::gpio
