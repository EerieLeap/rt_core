#pragma once

#include <chrono>

namespace eerie_leap::subsys::time {

class ITimeProvider {
public:
    virtual ~ITimeProvider() = default;

    std::chrono::system_clock::time_point virtual GetTime() = 0;
};

} // namespace eerie_leap::subsys::time
