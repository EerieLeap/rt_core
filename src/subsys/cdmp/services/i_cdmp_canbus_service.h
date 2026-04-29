#pragma once

#include <cstdint>
#include <span>
#include <memory>

#include "subsys/canbus/canbus_proxy.hpp"

namespace eerie_leap::subsys::cdmp::services {

using eerie_leap::subsys::canbus::CanbusProxy;

class ICdmpCanbusService {
public:
    virtual ~ICdmpCanbusService() = default;

    virtual void Initialize() = 0;
    virtual void Configure(std::shared_ptr<CanbusProxy> canbus) = 0;
    virtual void Start() = 0;
    virtual void Stop() = 0;
};

} // namespace eerie_leap::subsys::cdmp::services
