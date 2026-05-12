#pragma once

#include "i_time_provider.h"

namespace eerie_leap::subsys::time {

class RtcProvider : public ITimeProvider {
public:
    time_point GetTime() override;
};

} // namespace eerie_leap::subsys::time
