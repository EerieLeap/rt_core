#pragma once

#include <cstdint>
#include <span>
#include <memory>

#include "subsys/canbus/canbus_proxy.hpp"
#include "subsys/threading/i_service.h"

namespace eerie_leap::subsys::cdmp::services {

using eerie_leap::subsys::canbus::CanbusProxy;
using eerie_leap::subsys::threading::IService;

class ICdmpCanbusService : public IService {
public:
    virtual void Configure(std::shared_ptr<CanbusProxy> canbus) = 0;
};

} // namespace eerie_leap::subsys::cdmp::services
