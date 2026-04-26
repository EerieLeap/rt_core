#include <utility>
#include <exception>

#include <zephyr/logging/log.h>

#include "subsys/random/rng.h"

#include "canbus_com_service.h"

namespace eerie_leap::domain::canbus_com_domain::services {

using namespace eerie_leap::subsys::random;

LOG_MODULE_REGISTER(canbus_com_logger);

CanbusComService::CanbusComService(std::shared_ptr<CanbusService> canbus_service)
    : canbus_service_(std::move(canbus_service)) {

    cdmp_service_ = std::make_shared<CdmpService>(
        static_cast<CdmpDeviceType>(CONFIG_EERIE_LEAP_DOMAIN_CANBUS_COM_DEVICE_TYPE),
        Rng::Get32(true));

    if(cdmp_service_ == nullptr)
        throw std::runtime_error("Failed to create CDMP service");
}

void CanbusComService::Initialize() {
    cdmp_service_->Initialize();
}

bool CanbusComService::Start() {
    auto com_canbus = canbus_service_->GetComCanbus();
    if(!com_canbus)
        return false;

    cdmp_service_->Configure(com_canbus);
    cdmp_service_->Start();

    return true;
}

void CanbusComService::Stop() {
    if(!cdmp_service_)
        return;

    cdmp_service_->Reset();
}

void CanbusComService::UnsetCommandHandler(CanbusComCommandCode command_code) {
    if(!cdmp_service_)
        return;

    cdmp_service_->GetCommandService()->UnregisterCommandHandler(
        std::to_underlying(command_code));
}

void CanbusComService::SendCommand(ICanbusComCommand& command, CommandAckCallback callback, uint8_t device_id) {
    if(!cdmp_service_)
        return;

    if(callback) {
        cdmp_service_->GetCommandService()->SendCommand(
            device_id,
            std::to_underlying(command.GetCommandCode()),
            command.GetData(),
            [callback](uint8_t _, const CdmpResultCode result_code, std::span<const uint8_t> data) {
                callback(result_code == CdmpResultCode::SUCCESS);
            });
    } else {
        cdmp_service_->GetCommandService()->SendCommand(
            device_id,
            std::to_underlying(command.GetCommandCode()),
            command.GetData());
    }
}

} // namespace eerie_leap::domain::canbus_com_domain::services
