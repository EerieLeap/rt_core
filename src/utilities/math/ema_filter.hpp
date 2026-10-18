#pragma once

#include <concepts>
#include <cstdint>

namespace eerie_leap::utilities::math {

// Exponential Moving Average Filter
template<std::integral T>
class EmaFilter {
private:
    T filter_ = 0;
    T error_ = 0;

public:
    explicit EmaFilter(T value) :
        filter_(value),
        error_(0) {}

    T Filter(T val, uint8_t k2) {
        T sum = (val - filter_) + error_;
        T div = sum >> k2;
        error_ = sum - (div << k2);
        return filter_ += div;
    }

    T Get() {
        return filter_;
    }
};

} // namespace eerie_leap::utilities::math
