#pragma once

#include <concepts>
#include <cstdint>
#include <cstddef>

namespace eerie_leap::subsys::random {

// bool is the one unsigned integral type that has invalid object representations,
// so it cannot be filled from raw random bytes.
template<typename T>
concept RandomWord = std::unsigned_integral<T> && !std::same_as<T, bool>;

class Rng {
public:
    static void Get(void* dst, size_t len, bool secure = false);

    template<RandomWord T>
    static T Get(bool secure = false) {
        T value;
        Get(&value, sizeof(value), secure);

        return value;
    }
};

} // namespace eerie_leap::subsys::random
