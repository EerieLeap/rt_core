#pragma once

#include <functional>
#include <vector>
#include <span>
#include <unordered_map>
#include <memory>
#include <optional>

#include "subsys/threading/work_queue_thread.h"

#include "subsys/cdmp/models/cdmp_message.h"
#include "subsys/cdmp/services/cdmp_canbus_service_base.h"
#include "subsys/cdmp/services/command_service/cdmp_transaction_service.h"

namespace eerie_leap::subsys::cdmp::services {

using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::subsys::cdmp::utilities::CdmpResultCode;
using eerie_leap::subsys::cdmp::utilities::CdmpServiceCommandCode;
using eerie_leap::subsys::cdmp::models::CdmpCommandRequestMessage;
using eerie_leap::subsys::cdmp::models::CdmpCommandResponseMessage;
using eerie_leap::subsys::cdmp::services::command_service::CdmpTransactionService;
using eerie_leap::subsys::cdmp::services::command_service::CdmpTransactionCallback;

struct CdmpCommandResult {
    CdmpResultCode result_code;
    std::vector<uint8_t> data;
};

class CdmpCommandService final : public CdmpCanbusServiceBase {
public:
    using CommandHandler = std::function<CdmpCommandResult(uint8_t transaction_id, std::span<const uint8_t> data)>;

private:
    int canbus_handler_id_;
    int canbus_response_handler_id_;

    std::shared_ptr<WorkQueueThread> work_queue_thread_;
    std::unique_ptr<CdmpTransactionService> transaction_service_;

    std::unordered_map<uint8_t, CommandHandler> command_handlers_;

    void RegisterCanHandlers();
    void UnregisterCanHandlers();

    void OnDeviceStatusChanged(CdmpDeviceStatus old_status, CdmpDeviceStatus new_status) override;

    void ProcessRequestFrame(std::span<const uint8_t> frame_data);
    void ProcessServiceRequestFrame(const CdmpCommandRequestMessage& command);
    void ProcessResponseFrame(std::span<const uint8_t> frame_data);
    void SendCommandResponse(uint8_t target_device_id, const CdmpCommandResponseMessage& response);
    std::optional<CdmpCommandResult> NotifyCommandHandler(const CdmpCommandRequestMessage& command);

    bool IsValidServiceCommandCode(uint8_t command_code) const;
    bool IsValidUserCommandCode(uint8_t command_code) const;

    bool DoInitialize() override;
    bool DoStart() override;
    bool DoStop() override;

public:
    CdmpCommandService(
        std::shared_ptr<CdmpCanIdManager> can_id_manager,
        std::shared_ptr<CdmpDevice> device,
        std::shared_ptr<WorkQueueThread> work_queue_thread);

    ~CdmpCommandService();

    void RegisterCommandHandler(uint8_t command_code, CommandHandler handler);
    void RegisterUserCommandHandler(uint8_t command_code, CommandHandler handler);
    void RegisterServiceCommandHandler(CdmpServiceCommandCode command_code, CommandHandler handler);
    void UnregisterCommandHandler(uint8_t command_code);

    uint8_t SendCommand(
        uint8_t target_device_id,
        uint8_t command_code,
        std::span<const uint8_t> data = {},
        CdmpTransactionCallback callback = nullptr);
    uint8_t SendCommand(
        uint8_t target_device_id,
        CdmpServiceCommandCode command_code,
        std::span<const uint8_t> data = {},
        CdmpTransactionCallback callback = nullptr);
};

} // namespace eerie_leap::subsys::cdmp::services
