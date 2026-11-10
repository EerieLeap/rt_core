#pragma once

#include <cstdint>

namespace eerie_leap::utilities::math {

// Small seeded PRNG (Marsaglia xorshift32) for signals that must be reproducible from a seed.
// Not for anything that needs unpredictability.
class Xorshift32 {
private:
    static constexpr uint32_t ZERO_SEED_REPLACEMENT = 0x9E3779B9U;

    uint32_t state_;

public:
    // Xorshift never leaves the all-zero state, so a seed of 0 is replaced.
    explicit constexpr Xorshift32(uint32_t seed) :
        state_(seed != 0 ? seed : ZERO_SEED_REPLACEMENT) {}

    constexpr uint32_t Next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;

        return state_;
    }

    // [0, 1): 24 bits, so the result is exact in a float and never reaches 1.
    constexpr float NextFloat() {
        return static_cast<float>(Next() >> 8) * (1.0F / 16777216.0F);
    }

    // [min, max], both included; max <= min gives min.
    constexpr uint32_t NextInRange(uint32_t min, uint32_t max) {
        if(max <= min)
            return min;

        const uint64_t span = static_cast<uint64_t>(max) - min + 1;

        return min + static_cast<uint32_t>(Next() % span);
    }
};

} // namespace eerie_leap::utilities::math
