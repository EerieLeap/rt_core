#pragma once

#include <cstdint>

namespace eerie_leap::subsys::threading {

enum class ServiceState : uint8_t {
    STOPPED = 0,
    STARTING,
    RUNNING,
    PAUSED,
    STOPPING
};

} // namespace eerie_leap::subsys::threading
