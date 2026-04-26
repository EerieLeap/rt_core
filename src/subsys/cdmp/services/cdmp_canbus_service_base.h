#pragma once

#include <memory>

#include "subsys/canbus/canbus.h"

#include "subsys/cdmp/utilities/cdmp_can_id_manager.h"
#include "subsys/cdmp/utilities/cdmp_status_machine.h"
#include "subsys/cdmp/models/cdmp_device.h"
#include "subsys/cdmp/models/cdmp_message.h"

#include "i_cdmp_canbus_service.h"

namespace eerie_leap::subsys::cdmp::services {

using namespace eerie_leap::subsys::canbus;
using namespace eerie_leap::subsys::cdmp::models;
using namespace eerie_leap::subsys::cdmp::utilities;

class CdmpCanbusServiceBase : public ICdmpCanbusService {
private:
    int status_handler_id_;

protected:
    std::shared_ptr<Canbus> canbus_;
    std::shared_ptr<CdmpCanIdManager> can_id_manager_;
    std::shared_ptr<CdmpDevice> device_;

    virtual void OnDeviceStatusChanged(CdmpDeviceStatus old_status, CdmpDeviceStatus new_status);

public:
    CdmpCanbusServiceBase(
        std::shared_ptr<CdmpCanIdManager> can_id_manager,
        std::shared_ptr<CdmpDevice> device);

    virtual ~CdmpCanbusServiceBase();

    void Initialize() override {}
    void Configure(std::shared_ptr<Canbus> canbus) override;
    void Reset() override;
};

} // namespace eerie_leap::subsys::cdmp::services
