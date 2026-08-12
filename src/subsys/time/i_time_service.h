#pragma once

#include <chrono>

namespace eerie_leap::subsys::time {

class ITimeService {
public:
    virtual ~ITimeService() = default;

    std::chrono::system_clock::time_point virtual GetCurrentTime() = 0;
    std::chrono::system_clock::time_point virtual GetTimeSinceBoot() = 0;
};

} // namespace eerie_leap::subsys::time
