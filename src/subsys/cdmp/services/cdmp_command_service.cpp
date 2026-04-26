#include <zephyr/logging/log.h>

#include "cdmp_command_service.h"

LOG_MODULE_REGISTER(cdmp_command_service, LOG_LEVEL_INF);

namespace eerie_leap::subsys::cdmp::services {

CdmpCommandService::CdmpCommandService(
    std::shared_ptr<CdmpCanIdManager> can_id_manager,
    std::shared_ptr<CdmpDevice> device,
    std::shared_ptr<WorkQueueThread> work_queue_thread)
        : CdmpCanbusServiceBase(std::move(can_id_manager), std::move(device))
        , canbus_handler_id_(-1)
        , canbus_response_handler_id_(-1)
        , work_queue_thread_(std::move(work_queue_thread)) {

    transaction_service_ = std::make_unique<CdmpTransactionService>(work_queue_thread_, device_);
}

void CdmpCommandService::Initialize() {
    transaction_service_->Initialize();
}

CdmpCommandService::~CdmpCommandService() {
    Stop();
}

void CdmpCommandService::Start() {
    if(atomic_get(&is_running_) != 0)
        return;

    atomic_set(&is_running_, 1);
    RegisterCanHandlers();

    LOG_INF("CDMP Command Service started");
}

void CdmpCommandService::Stop() {
    if(atomic_get(&is_running_) == 0)
        return;

    atomic_set(&is_running_, 0);
    UnregisterCanHandlers();

    LOG_INF("CDMP Command Service stopped");
}

void CdmpCommandService::RegisterCanHandlers() {
    if (!canbus_) return;

    canbus_handler_id_ = canbus_->RegisterFrameReceivedHandler(
        can_id_manager_->GetCommandRequestCanId(),
        [this](const CanFrame& frame) {
            work_queue_thread_->Run([this, frame]() { ProcessRequestFrame(frame.data); }); });

    canbus_response_handler_id_ = canbus_->RegisterFrameReceivedHandler(
        can_id_manager_->GetCommandResponseCanId(),
        [this](const CanFrame& frame) {
            work_queue_thread_->Run([this, frame]() { ProcessResponseFrame(frame.data); }); });
}

void CdmpCommandService::UnregisterCanHandlers() {
    if (!canbus_) return;

    if (canbus_handler_id_ >= 0) {
        canbus_->RemoveFrameReceivedHandler(can_id_manager_->GetCommandRequestCanId(), canbus_handler_id_);
        canbus_handler_id_ = -1;
    }

    if (canbus_response_handler_id_ >= 0) {
        canbus_->RemoveFrameReceivedHandler(can_id_manager_->GetCommandResponseCanId(), canbus_response_handler_id_);
        canbus_response_handler_id_ = -1;
    }
}

void CdmpCommandService::OnDeviceStatusChanged(CdmpDeviceStatus old_status, CdmpDeviceStatus new_status) {
    switch(new_status) {
        case CdmpDeviceStatus::ONLINE:
            transaction_service_->Start();
            break;

        default:
            transaction_service_->Stop();
            break;
    }
}

void CdmpCommandService::ProcessRequestFrame(std::span<const uint8_t> frame_data) {
    try {
        CdmpCommandRequestMessage command = CdmpCommandRequestMessage::FromCanFrame(frame_data);

        if(command.target_device_id != device_->GetDeviceId()
            && command.target_device_id != CdmpDevice::DEVICE_ID_BROADCAST) {

            return;
        }

        if(IsValidServiceCommandCode(command.command_code)) {
            ProcessServiceRequestFrame(command);
            return;
        }

        if(device_->GetStatus() != CdmpDeviceStatus::ONLINE)
            throw std::runtime_error("Device is not online");

        if(!IsValidUserCommandCode(command.command_code))
            throw std::runtime_error("Invalid user command code");

        auto result = NotifyCommandHandler(command);

        if(result.has_value()) {
            LOG_DBG("Processed command %d for transaction %d",
                command.command_code, command.transaction_id);

            CdmpCommandResponseMessage response {
                .source_device_id = device_->GetDeviceId(),
                .command_code = command.command_code,
                .transaction_id = command.transaction_id,
                .result_code = result.value().result_code,
                .data = result.value().data
            };

            SendCommandResponse(command.target_device_id, response);
        } else if(command.target_device_id == device_->GetDeviceId()) {
            LOG_WRN("No handler registered for command %d", command.command_code);

            // Send error response
            CdmpCommandResponseMessage response {
                .source_device_id = device_->GetDeviceId(),
                .command_code = command.command_code,
                .transaction_id = command.transaction_id,
                .result_code = CdmpResultCode::UNSUPPORTED_COMMAND,
                .data = {}
            };

            SendCommandResponse(command.target_device_id, response);
        }
    } catch (const std::exception& e) {
        LOG_ERR("Error processing command frame: %s", e.what());
    }
}

void CdmpCommandService::ProcessServiceRequestFrame(const CdmpCommandRequestMessage& command) {
    if(command.command_code == std::to_underlying(CdmpServiceCommandCode::STATUS_REQUEST)) {
        // Status request is allowed even when device is not online
        CdmpCommandResponseStatusMessage message(
            device_->GetDeviceId(),
            command.transaction_id,
            CdmpResultCode::SUCCESS,
            device_->GetStatus(),
            device_->GetProtocolVersion(),
            device_->GetHealthStatus()
        );

        NotifyCommandHandler(command);
        SendCommandResponse(command.target_device_id, message);
    } else if(device_->GetStatus() == CdmpDeviceStatus::ONLINE) {
        NotifyCommandHandler(command);
    }
}

void CdmpCommandService::ProcessResponseFrame(std::span<const uint8_t> frame_data) {
    try {
        CdmpCommandResponseMessage response = CdmpCommandResponseMessage::FromCanFrame(frame_data);
        LOG_DBG("Received command response for transaction %d, code %d",
            response.transaction_id, std::to_underlying(response.result_code));

        transaction_service_->CompleteTransaction(response);
    } catch (const std::exception& e) {
        LOG_ERR("Error processing command response frame: %s", e.what());
    }
}

void CdmpCommandService::SendCommandResponse(uint8_t target_device_id, const CdmpCommandResponseMessage& response) {
    try {
        auto frame = response.ToCanFrame();
        uint32_t can_id = can_id_manager_->GetCommandResponseCanId();

        if(target_device_id != device_->GetDeviceId())
            k_msleep(device_->GetStaggeredMessageDelay());

        canbus_->SendFrame(can_id, frame);

        LOG_DBG("Sent command response for transaction %d", response.transaction_id);
    } catch (const std::exception& e) {
        LOG_ERR("Error sending command response: %s", e.what());
    }
}

std::optional<CdmpCommandResult> CdmpCommandService::NotifyCommandHandler(const CdmpCommandRequestMessage& command) {
    if(command_handlers_.contains(command.command_code))
        return command_handlers_.at(command.command_code)(command.transaction_id, command.data);

    return std::nullopt;
}

void CdmpCommandService::RegisterCommandHandler(uint8_t command_code, CommandHandler handler) {
    if(command_code == 0)
        throw std::runtime_error("Command code 0 is not allowed");

    command_handlers_[command_code] = std::move(handler);
    LOG_DBG("Registered handler for command %d", command_code);
}

void CdmpCommandService::RegisterUserCommandHandler(uint8_t command_code, CommandHandler handler) {
    if(!IsValidUserCommandCode(command_code))
        throw std::runtime_error("Invalid user command code");

    RegisterCommandHandler(command_code, handler);
}

void CdmpCommandService::RegisterServiceCommandHandler(CdmpServiceCommandCode command_code, CommandHandler handler) {
    if(!IsValidServiceCommandCode(std::to_underlying(command_code)))
        throw std::runtime_error("Invalid service command code");

    RegisterCommandHandler(std::to_underlying(command_code), handler);
}

void CdmpCommandService::UnregisterCommandHandler(uint8_t command_code) {
    command_handlers_.erase(command_code);
    LOG_DBG("Unregistered handler for command %d", command_code);
}

uint8_t CdmpCommandService::SendCommand(
    uint8_t target_device_id,
    uint8_t command_code,
    std::span<const uint8_t> data,
    CdmpTransactionCallback callback) {

    try {
        if(command_code == 0)
            throw std::runtime_error("Command code 0 is not allowed");

        uint8_t transaction_id = transaction_service_->StartTransaction(
            command_code,
            CONFIG_EERIE_LEAP_CDMP_CMD_TRANSACTION_TIMEOUT_MS,
            callback);

        CdmpCommandRequestMessage command{
            .target_device_id = target_device_id,
            .command_code = command_code,
            .transaction_id = transaction_id,
            .data = std::vector<uint8_t>(data.begin(), data.end())
        };

        auto frame_data = command.ToCanFrame();
        uint32_t frame_id = can_id_manager_->GetCommandRequestCanId();
        canbus_->SendFrame(frame_id, frame_data);

        LOG_DBG("Sent command %d to device %d, transaction %d",
            command_code, target_device_id, transaction_id);

        return transaction_id;
    } catch (const std::exception& e) {
        LOG_ERR("Error sending command: %s", e.what());
        return 0;
    }
}

uint8_t CdmpCommandService::SendCommand(
        uint8_t target_device_id,
        CdmpServiceCommandCode command_code,
        std::span<const uint8_t> data,
        CdmpTransactionCallback callback) {

    return SendCommand(target_device_id, std::to_underlying(command_code), data, callback);
}

bool CdmpCommandService::IsValidServiceCommandCode(uint8_t command_code) const {
    auto service_command_code = static_cast<CdmpServiceCommandCode>(command_code);

    return service_command_code == CdmpServiceCommandCode::STATUS_REQUEST
        || service_command_code == CdmpServiceCommandCode::RESET_DEVICE
        || service_command_code == CdmpServiceCommandCode::GET_CONFIG_CRC
        || service_command_code == CdmpServiceCommandCode::GET_CONFIG;
}

bool CdmpCommandService::IsValidUserCommandCode(uint8_t command_code) const {
    return command_code >= CdmpConstants::USER_COMMAND_CODE_MIN
        && command_code <= CdmpConstants::USER_COMMAND_CODE_MAX;
}

} // namespace eerie_leap::subsys::cdmp::services
