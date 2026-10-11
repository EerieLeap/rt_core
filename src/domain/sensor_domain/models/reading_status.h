#pragma once

#include <cstdint>

namespace eerie_leap::domain::sensor_domain::models {

enum class ReadingStatus : uint8_t {
    UNINITIALIZED,
    RAW,
    INTERPOLATED,
    EXPRESSION_EVALUATED,
    PROCESSED,
    ERROR,
};

}

// namespace eerie_leap::domain::sensor_domain::models
