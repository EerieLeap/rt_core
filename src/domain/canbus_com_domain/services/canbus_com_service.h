#pragma once

#include <concepts>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "utilities/concepts/concepts.h"

#include "subsys/cdmp/services/cdmp_service.h"
#include "subsys/cdmp/utilities/constants.h"
#include "subsys/smp/can/smp_can_transport.h"
#include "subsys/smp/i_smp_forwarder.h"
#include "subsys/threading/service_base.h"

#include "domain/canbus_com_domain/commands/i_canbus_com_command.h"
#include "domain/canbus_com_domain/services/i_network_state_transport.h"
#include "domain/canbus_domain/services/canbus_service.h"

namespace eerie_leap::domain::canbus_com_domain::services {

namespace concepts = eerie_leap::utilities::concepts;

using eerie_leap::subsys::cdmp::models::CdmpDeviceInfo;
using eerie_leap::subsys::cdmp::models::CdmpHeartbeatMessage;
using eerie_leap::subsys::cdmp::services::CdmpCommandResult;
using eerie_leap::subsys::cdmp::services::CdmpDeviceEvent;
using eerie_leap::subsys::cdmp::services::CdmpNetworkService;
using eerie_leap::subsys::cdmp::services::CdmpService;
using eerie_leap::subsys::cdmp::services::ICdmpNetworkInfo;
using eerie_leap::subsys::cdmp::utilities::CdmpConstants;
using eerie_leap::subsys::cdmp::utilities::CdmpDeviceStatus;
using eerie_leap::subsys::cdmp::utilities::CdmpResultCode;
using eerie_leap::subsys::smp::ISmpForwarder;
using eerie_leap::subsys::smp::ISmpResponseSink;
using eerie_leap::subsys::smp::SmpPacket;
using eerie_leap::subsys::smp::can::SmpCanTransport;
using eerie_leap::subsys::threading::ServiceBase;
using eerie_leap::subsys::threading::ServiceState;

using eerie_leap::domain::canbus_com_domain::commands::CanbusComCommandCode;
using eerie_leap::domain::canbus_com_domain::commands::CanbusComCommandResultBase;
using eerie_leap::domain::canbus_com_domain::commands::ICanbusComCommand;
using eerie_leap::domain::canbus_domain::services::CanbusService;

/**
 * @brief Runs CDMP and the SMP-CAN transport on the COM channel, across bus reconfigurations.
 *
 * As an ISmpForwarder it sends SMP packets to the other CDMP devices. As an INetworkStateTransport it
 * shares state records with them through CDMP's state service, which keeps this unit's records across
 * the stop and start of every reconfiguration and announces them when the node is back ONLINE.
 */
class CanbusComService : public ServiceBase<>, public ISmpForwarder, public INetworkStateTransport {
private:
    std::shared_ptr<CanbusService> canbus_service_;
    std::shared_ptr<CdmpService> cdmp_service_;
    std::shared_ptr<SmpCanTransport> smp_transport_;
    int cdmp_status_handler_id_ = -1;
    int canbus_reset_handler_id_ = -1;
    int canbus_updated_handler_id_ = -1;

    bool DoInitialize() override;
    bool DoStart() override;
    bool DoStop() override;

    /**
     * @brief Binds SMP to the claimed CDMP ID in ONLINE and VERSION_MISMATCH, unbinds otherwise.
     *
     * VERSION_MISMATCH keeps SMP so a unit with another CDMP version can still be managed.
     */
    void OnCdmpStatusChanged(CdmpDeviceStatus status);

public:
    using CommandAckCallback = std::function<void(bool success)>;
    template<concepts::SpanConstructible TResponse>
    using CommandDataResponseCallback = std::function<void(bool success, std::optional<TResponse> response)>;

    template<concepts::SpanConstructible TRequest>
    using CommandDataRequestCallback = std::function<bool(std::optional<TRequest> request)>;

    template<concepts::SpanConstructible TRequest>
    using CommandDataRequestWithResponseCallback =
        std::function<std::optional<CanbusComCommandResultBase>(std::optional<TRequest> request)>;

    using DeviceEventHandler = CdmpNetworkService::DeviceEventHandler;
    using HeartbeatHandler = CdmpNetworkService::HeartbeatHandler;

    CanbusComService(std::shared_ptr<CanbusService> canbus_service);
    ~CanbusComService() override;

    /** @brief The features as heartbeats and discovery responses send them now. Any thread. */
    [[nodiscard]] uint16_t GetFeatures() const;
    /**
     * @brief Sets or clears LOGGING in what this unit sends. The bit starts cleared; a unit sets it
     * while it has a log writer and a mounted log volume (CanbusComLoggingAvailabilityReporter).
     * Setting it does nothing when the build lacks EERIE_LEAP_FEATURES_LOGGING. Any thread; the next
     * heartbeat (within 3 s) carries it, and it survives a bus reconfiguration.
     */
    void SetLoggingAvailable(bool is_available);

    /** @brief This unit and the other devices, as CDMP sees them. */
    [[nodiscard]] std::shared_ptr<const ICdmpNetworkInfo> GetNetworkInfo() const {
        return cdmp_service_;
    }

    /** @brief The claimed CDMP ID while SMP is bound to it, 0 otherwise. */
    [[nodiscard]] uint8_t GetAddress() const override {
        return smp_transport_->GetAddress();
    }
    /** @brief Whether @p target is another device that CDMP sees online, while SMP is bound. */
    [[nodiscard]] bool IsReachable(uint8_t target) const override;
    bool Forward(uint8_t target, SmpPacket packet) override;
    void SetResponseSink(std::shared_ptr<ISmpResponseSink> response_sink) override;

    using INetworkStateTransport::SetLocalState;
    /** @brief Also while stopped, or before Initialize(); the record is sent on the next ONLINE. */
    bool SetLocalState(CanbusComStateType state_type, const CanbusComStateData& data) override;
    void ClearLocalState(CanbusComStateType state_type) override;
    /** @brief A handler registered before Initialize() gets its first call once the CDMP work queue runs. */
    int RegisterStateHandler(CanbusComStateType state_type, StateHandler handler) override;
    void UnregisterStateHandler(int handler_id) override;
    size_t GetRemoteStates(CanbusComStateType state_type, std::span<RemoteStateRecord> records) const override;

    /**
     * @brief Reports other devices joining, restarting and leaving the device table, as
     * CdmpNetworkService::RegisterDeviceEventHandler() does.
     *
     * The handler runs on the CDMP work queue, under the network service's handler lock, so it must
     * not block, throw, or register or unregister handlers. It stays registered across the restarts
     * of this node (a CAN reconfiguration), which report no event of their own: they drop the table.
     * CDMP cannot tell a unit that rebooted from one whose COM service restarted, so both are
     * CdmpDeviceEvent::RESTARTED.
     *
     * @return A handler ID for UnregisterDeviceEventHandler(), or -1 without a CDMP service.
     */
    int RegisterDeviceEventHandler(DeviceEventHandler handler);
    /** @brief Once this returns, the handler is not running and is not called again; not from a handler. */
    void UnregisterDeviceEventHandler(int handler_id);

    /**
     * @brief Passes on each heartbeat of a device in the table, after the state service has checked its
     * digest against the copies of that device's records.
     *
     * Runs under the rules of RegisterDeviceEventHandler(). A copy that a matching digest confirms is
     * current again by the time the handler is called.
     *
     * @return A handler ID for UnregisterHeartbeatHandler(), or -1 without a CDMP service.
     */
    int RegisterHeartbeatHandler(HeartbeatHandler handler);
    /** @brief Once this returns, the handler is not running and is not called again; not from a handler. */
    void UnregisterHeartbeatHandler(int handler_id);

    // NOTE: A handler runs on the CDMP work queue and must not block, in both overloads. For a usage
    // example, see LoggingCommandHandler (domain/logging_domain/control/logging_command_handler.cpp),
    // which returns whether it accepted the LOGGING command, as this overload takes it, and leaves
    // the outcome to the shared status.
    template<concepts::SpanConstructible TRequest>
    void SetCommandHandler(CanbusComCommandCode command_code, CommandDataRequestCallback<TRequest> callback) {
        if(!cdmp_service_)
            return;

        cdmp_service_->GetCommandService()->RegisterCommandHandler(
            std::to_underlying(command_code), [callback](uint8_t _, std::span<const uint8_t> data) {
                bool result = callback(data.empty() ? std::nullopt : std::optional<TRequest>(data));

                return CdmpCommandResult{ result ? CdmpResultCode::SUCCESS : CdmpResultCode::FAILURE, {} };
            }
        );
    }

    // NOTE: This overload answers with a result of its own; return std::nullopt to answer FAILURE.
    template<concepts::SpanConstructible TRequest>
    void
    SetCommandHandler(CanbusComCommandCode command_code, CommandDataRequestWithResponseCallback<TRequest> callback) {
        if(!cdmp_service_)
            return;

        cdmp_service_->GetCommandService()->RegisterCommandHandler(
            std::to_underlying(command_code), [callback](uint8_t _, std::span<const uint8_t> data) {
                auto result = callback(data.empty() ? std::nullopt : std::optional<TRequest>(data));

                if(result.has_value()) {
                    return CdmpCommandResult{ CdmpResultCode::SUCCESS, result->GetData() };
                }

                return CdmpCommandResult{ CdmpResultCode::FAILURE, {} };
            }
        );
    }

    void UnsetCommandHandler(CanbusComCommandCode command_code);

    void SendCommand(
        ICanbusComCommand& command,
        CommandAckCallback callback = nullptr,
        uint8_t device_id = CdmpConstants::COMMAND_BROADCAST_ID
    );

    template<concepts::SpanConstructible TResponse>
    void SendCommand(
        ICanbusComCommand& command,
        CommandDataResponseCallback<TResponse> callback,
        uint8_t device_id = CdmpConstants::COMMAND_BROADCAST_ID
    ) {

        if(!cdmp_service_)
            return;

        SendCommand(
            command,
            [callback](uint8_t _, const CdmpResultCode result_code, std::span<const uint8_t> data) {
                callback(
                    result_code == CdmpResultCode::SUCCESS,
                    result_code == CdmpResultCode::SUCCESS ? std::optional<TResponse>(data) : std::nullopt
                );
            },
            device_id
        );
    }
};

} // namespace eerie_leap::domain::canbus_com_domain::services
