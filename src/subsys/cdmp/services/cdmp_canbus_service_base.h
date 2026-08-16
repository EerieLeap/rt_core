#pragma once

#include <memory>

#include "subsys/canbus/canbus_proxy.hpp"
#include "subsys/threading/service_base.h"

#include "subsys/cdmp/utilities/cdmp_can_id_manager.h"
#include "subsys/cdmp/utilities/cdmp_status_machine.h"
#include "subsys/cdmp/models/cdmp_device.h"
#include "subsys/cdmp/models/cdmp_message.h"

#include "i_cdmp_canbus_service.h"

namespace eerie_leap::subsys::cdmp::services {

using eerie_leap::subsys::threading::ServiceBase;
using eerie_leap::subsys::threading::ServiceState;
using eerie_leap::subsys::cdmp::models::CdmpDevice;
using eerie_leap::subsys::cdmp::utilities::CdmpDeviceStatus;
using eerie_leap::subsys::cdmp::utilities::CdmpCanIdManager;

class CdmpCanbusServiceBase : public ServiceBase<ICdmpCanbusService> {
private:
    int status_handler_id_;

protected:
    std::shared_ptr<CanbusProxy> canbus_;
    std::shared_ptr<CdmpCanIdManager> can_id_manager_;
    std::shared_ptr<CdmpDevice> device_;

    virtual void OnDeviceStatusChanged(CdmpDeviceStatus old_status, CdmpDeviceStatus new_status);

public:
    CdmpCanbusServiceBase(
        std::shared_ptr<CdmpCanIdManager> can_id_manager,
        std::shared_ptr<CdmpDevice> device);

    virtual ~CdmpCanbusServiceBase();

    void Configure(std::shared_ptr<CanbusProxy> canbus) override;
};

} // namespace eerie_leap::subsys::cdmp::services
