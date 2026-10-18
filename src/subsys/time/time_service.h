#pragma once

#include <memory>

#include "i_time_provider.h"
#include "i_time_service.h"

namespace eerie_leap::subsys::time {

class TimeService : public ITimeService {
private:
    std::shared_ptr<ITimeProvider> rtc_provider_;
    std::shared_ptr<ITimeProvider> boot_elapsed_time_provider_;

public:
    TimeService(std::shared_ptr<ITimeProvider> rtc_provider, std::shared_ptr<ITimeProvider> boot_elapsed_time_provider);

    static void Initialize();

    std::chrono::system_clock::time_point GetCurrentTime() override;
    std::chrono::system_clock::time_point GetTimeSinceBoot() override;
};

} // namespace eerie_leap::subsys::time
