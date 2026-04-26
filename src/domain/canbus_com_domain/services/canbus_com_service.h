#pragma once

#include <cstdint>
#include <vector>
#include <span>
#include <concepts>
#include <optional>

#include "utilities/concepts/concepts.h"

#include "subsys/cdmp/utilities/constants.h"
#include "subsys/cdmp/services/cdmp_service.h"

#include "domain/canbus_domain/services/canbus_service.h"
#include "domain/canbus_com_domain/commands/i_canbus_com_command.h"

namespace eerie_leap::domain::canbus_com_domain::services {

using namespace eerie_leap::utilities::concepts;

using namespace eerie_leap::subsys::cdmp::utilities;
using namespace eerie_leap::subsys::cdmp::services;

using namespace eerie_leap::domain::canbus_domain::services;
using namespace eerie_leap::domain::canbus_com_domain::commands;

class CanbusComService {
private:
    std::shared_ptr<CanbusService> canbus_service_;
    std::shared_ptr<CdmpService> cdmp_service_;

public:
    using CommandAckCallback = std::function<void(bool success)>;
    template<SpanConstructible TResponse>
    using CommandDataResponseCallback = std::function<void(bool success, std::optional<TResponse> response)>;

    template<SpanConstructible TRequest>
    using CommandDataRequestCallback = std::function<bool(std::optional<TRequest> request)>;

    template<SpanConstructible TRequest>
    using CommandDataRequestWithResponseCallback = std::function<std::optional<CanbusComCommandResultBase>(std::optional<TRequest> request)>;

    CanbusComService(std::shared_ptr<CanbusService> canbus_service);
    virtual ~CanbusComService() = default;

    void Initialize();
    bool Start();
    void Stop();

    template<SpanConstructible TRequest>
    void SetCommandHandler(CanbusComCommandCode command_code, CommandDataRequestCallback<TRequest> callback) {
        if(!cdmp_service_)
            return;

        cdmp_service_->GetCommandService()->RegisterCommandHandler(
            std::to_underlying(command_code),
            [callback](uint8_t _, std::span<const uint8_t> data) {
                bool result = callback(data.empty() ? std::nullopt : std::optional<TRequest>(data));

                return CdmpCommandResult {
                    result ? CdmpResultCode::SUCCESS : CdmpResultCode::FAILURE,
                    {}
                };
            });
    }

    // NOTE: Usage example:
    // canbus_com_service_->SetCommandHandler<CanbusComLoggingCommand>(
    //     CanbusComCommandCode::LOGGING,
    //     [this](std::optional<CanbusComLoggingCommand> command) {
    //         std::optional<CanbusComCommandResultBase> result = std::nullopt;
    //         if(!command.has_value())
    //             return result;

    //         if(command.value().IsStart())
    //             result = std::make_optional<CanbusComLoggingCommandResult>(LogWriterStart() == 0);
    //         else
    //             result = std::make_optional<CanbusComLoggingCommandResult>(LogWriterStop() == 0);

    //         return result;
    //     });
    template<SpanConstructible TRequest>
    void SetCommandHandler(CanbusComCommandCode command_code, CommandDataRequestWithResponseCallback<TRequest> callback) {
        if(!cdmp_service_)
            return;

        cdmp_service_->GetCommandService()->RegisterCommandHandler(
            std::to_underlying(command_code),
            [callback](uint8_t _, std::span<const uint8_t> data) {
                auto result = callback(data.empty() ? std::nullopt : std::optional<TRequest>(data));

                if (result.has_value()) {
                    return CdmpCommandResult {
                        CdmpResultCode::SUCCESS,
                        result->GetData()
                    };
                }

                return CdmpCommandResult {
                    CdmpResultCode::FAILURE,
                    {}
                };
            });
    }

    void UnsetCommandHandler(CanbusComCommandCode command_code);

    void SendCommand(
        ICanbusComCommand& command,
        CommandAckCallback callback = nullptr,
        uint8_t device_id = CdmpConstants::COMMAND_BROADCAST_ID);

    template<SpanConstructible TResponse>
    void SendCommand(
        ICanbusComCommand& command,
        CommandDataResponseCallback<TResponse> callback,
        uint8_t device_id = CdmpConstants::COMMAND_BROADCAST_ID) {

        if(!cdmp_service_)
            return;

        SendCommand(command, [callback](uint8_t _, const CdmpResultCode result_code, std::span<const uint8_t> data) {
            callback(
                result_code == CdmpResultCode::SUCCESS,
                result_code == CdmpResultCode::SUCCESS ? std::optional<TResponse>(data) : std::nullopt);
        }, device_id);
    }
};

} // namespace eerie_leap::domain::canbus_com_domain::services
