#pragma once

#include <cstdint>

namespace eerie_leap::subsys::gpio {

enum class GpioEdge : uint8_t {
    ACTIVE,
    INACTIVE,
    BOTH
};

}  // namespace eerie_leap::subsys::gpio
