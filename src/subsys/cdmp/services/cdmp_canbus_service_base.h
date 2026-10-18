#pragma once

#include <memory>

#include "subsys/canbus/canbus_proxy.hpp"
#include "subsys/threading/service_base.h"

#include "subsys/cdmp/models/cdmp_device.h"
#include "subsys/cdmp/models/cdmp_message.h"
#include "subsys/cdmp/utilities/cdmp_can_id_manager.h"
#include "subsys/cdmp/utilities/cdmp_status_machine.h"

#include "i_cdmp_canbus_service.h"

namespace eerie_leap::subsys::cdmp::services {

using eerie_leap::subsys::canbus::CanFrameHandler;
using eerie_leap::subsys::canbus::CanId;
using eerie_leap::subsys::cdmp::models::CdmpDevice;
using eerie_leap::subsys::cdmp::utilities::CdmpCanIdManager;
using eerie_leap::subsys::cdmp::utilities::CdmpDeviceStatus;
using eerie_leap::subsys::threading::ServiceBase;
using eerie_leap::subsys::threading::ServiceState;

class CdmpCanbusServiceBase : public ServiceBase<ICdmpCanbusService> {
private:
    int status_handler_id_;

protected:
    std::shared_ptr<CanbusProxy> canbus_;
    std::shared_ptr<CdmpCanIdManager> can_id_manager_;
    std::shared_ptr<CdmpDevice> device_;

    virtual void OnDeviceStatusChanged(CdmpDeviceStatus old_status, CdmpDeviceStatus new_status);

    /**
     * @brief Registers @p handler for the frames with @p can_id, one of the Base+0..Base+5 IDs.
     *
     * The services share the CDMP block filter when the base allows it
     * (CdmpCanIdManager::GetReceiveFilter), so CDMP takes one controller filter. When another
     * registration overlaps the block, the exact filter is used instead. The handler only sees
     * frames with @p can_id either way.
     *
     * @return The handler ID, or a negative Canbus error.
     */
    int RegisterReceiveHandler(const CanId& can_id, CanFrameHandler handler);

public:
    CdmpCanbusServiceBase(std::shared_ptr<CdmpCanIdManager> can_id_manager, std::shared_ptr<CdmpDevice> device);

    ~CdmpCanbusServiceBase() override;
    CdmpCanbusServiceBase(const CdmpCanbusServiceBase&) = delete;
    CdmpCanbusServiceBase& operator=(const CdmpCanbusServiceBase&) = delete;
    CdmpCanbusServiceBase(CdmpCanbusServiceBase&&) = delete;
    CdmpCanbusServiceBase& operator=(CdmpCanbusServiceBase&&) = delete;

    void Configure(std::shared_ptr<CanbusProxy> canbus) override;
};

} // namespace eerie_leap::subsys::cdmp::services
