#pragma once

#include <chrono>

namespace eerie_leap::subsys::time {

using time_point = std::chrono::system_clock::time_point;

class ITimeProvider {
public:
    virtual ~ITimeProvider() = default;

    time_point virtual GetTime() = 0;
};

} // namespace eerie_leap::subsys::time
