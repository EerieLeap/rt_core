#include <utility>
#include <exception>

#include <zephyr/logging/log.h>

#include "subsys/cdmp/utilities/cdmp_uid.h"

#include "canbus_com_service.h"

namespace eerie_leap::domain::canbus_com_domain::services {

using namespace eerie_leap::subsys::cdmp::models;
using eerie_leap::subsys::cdmp::utilities::CdmpUid;
using eerie_leap::subsys::smp::can::SmpCanIdLayout;

LOG_MODULE_REGISTER(canbus_com_logger);

CanbusComService::CanbusComService(std::shared_ptr<CanbusService> canbus_service)
    : canbus_service_(std::move(canbus_service)) {

    cdmp_service_ = std::make_shared<CdmpService>(
        static_cast<CdmpDeviceType>(CONFIG_EERIE_LEAP_DOMAIN_CANBUS_COM_DEVICE_TYPE),
        CdmpUid::Generate());

    if(cdmp_service_ == nullptr)
        throw std::runtime_error("Failed to create CDMP service");

    smp_transport_ = std::make_shared<SmpCanTransport>(cdmp_service_->GetWorkQueueThread());

    cdmp_status_handler_id_ = cdmp_service_->RegisterStatusChangedHandler(
        [this](CdmpDeviceStatus /*old_status*/, CdmpDeviceStatus new_status) {
            OnCdmpStatusChanged(new_status);
        });

    canbus_service_->RegisterConfigurationResetHandler(
        [this]() { Stop(); });

    canbus_service_->RegisterConfigurationUpdatedHandler(
        [this]() { Start(); });
}

CanbusComService::~CanbusComService() {
    cdmp_service_->UnregisterStatusChangedHandler(cdmp_status_handler_id_);
}

bool CanbusComService::DoInitialize() {
    return cdmp_service_->Initialize() && smp_transport_->Initialize();
}

bool CanbusComService::DoStart() {
    auto com_canbus = canbus_service_->GetComCanbus();
    if(!com_canbus)
        return false;

    const auto com_configuration = canbus_service_->GetComConfiguration();

    smp_transport_->Configure(
        com_canbus, com_configuration.smp_can_id_base, com_configuration.smp_bus_share_percent);
    cdmp_service_->Configure(com_canbus, com_configuration.cdmp_base_can_id);

    return cdmp_service_->Start();
}

bool CanbusComService::DoStop() {
    if(!cdmp_service_)
        return false;

    const bool is_stopped = cdmp_service_->Stop();
    smp_transport_->Unbind();

    return is_stopped;
}

void CanbusComService::OnCdmpStatusChanged(CdmpDeviceStatus status) {
    const uint8_t device_id = cdmp_service_->GetDevice()->GetDeviceId();

    if((status == CdmpDeviceStatus::ONLINE || status == CdmpDeviceStatus::VERSION_MISMATCH)
        && SmpCanIdLayout::IsValidAddress(device_id)) {

        smp_transport_->Bind(device_id);
    } else {
        smp_transport_->Unbind();
    }
}

bool CanbusComService::IsReachable(uint8_t target) const {
    const uint8_t address = smp_transport_->GetAddress();

    return address != 0 && target != address && cdmp_service_->IsDeviceOnline(target);
}

bool CanbusComService::Forward(uint8_t target, SmpPacket packet) {
    return smp_transport_->Forward(target, std::move(packet));
}

void CanbusComService::SetResponseSink(std::shared_ptr<ISmpResponseSink> response_sink) {
    smp_transport_->SetResponseSink(std::move(response_sink));
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
