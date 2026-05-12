#pragma once

#include <chrono>

namespace eerie_leap::subsys::time {

using time_point = std::chrono::system_clock::time_point;

class ITimeService {
public:
    virtual ~ITimeService() = default;

    time_point virtual GetCurrentTime() = 0;
    time_point virtual GetTimeSinceBoot() = 0;
};

} // namespace eerie_leap::subsys::time
