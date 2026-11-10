#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#include "domain/can_monitor_domain/models/can_monitor_types.h"
#include "domain/can_monitor_domain/utilities/can_record.h"
#include "domain/canbus_domain/services/canbus_service.h"
#include "subsys/canbus/canbus.h"
#include "subsys/canbus/canbus_proxy.hpp"
#include "subsys/threading/service_base.h"

#include "can_monitor_link.h"
#include "can_monitor_sender.h"
#include "can_monitor_session.h"
#include "can_synthetic_source.h"

namespace eerie_leap::domain::can_monitor_domain::services {

using eerie_leap::domain::can_monitor_domain::models::CanbusType;
using eerie_leap::domain::can_monitor_domain::models::CanMonitorError;
using eerie_leap::domain::can_monitor_domain::models::CanMonitorMode;
using eerie_leap::domain::can_monitor_domain::models::CanMonitorOpenAnswer;
using eerie_leap::domain::can_monitor_domain::models::CanMonitorOpenRequest;
using eerie_leap::domain::can_monitor_domain::models::CanSyntheticParameters;
using eerie_leap::domain::can_monitor_domain::utilities::CanEndReason;
using eerie_leap::domain::canbus_domain::services::CanbusService;
using eerie_leap::subsys::canbus::CanbusProxy;
using eerie_leap::subsys::threading::ServiceBase;

/**
 * @brief The CAN monitor's sessions: OPEN, CLOSE and the sender that streams them.
 *
 * - Initialize() starts the sender, which keeps running until the service is destroyed so that sessions
 *   ended by Stop() still drain. Start() lets OPEN in; Stop() ends every session with the reason
 *   "configuration stored" (§3.5), which is how CanbusController::RegisterDependentService() uses it
 *   before a new CAN configuration is applied.
 * - At most EERIE_LEAP_CAN_MONITOR_MAX_SESSIONS sessions, at most one a port (255 for the synthetic one),
 *   all under one k_mutex that the tap never takes. The lock is taken before the sender's.
 * - Open() answers the same token to a repeated OPEN (same port, mode, type, bitrates) while no later
 *   request has carried the token: a client that lost the answer gets its session, not BUSY.
 *
 * Phase 2 supports TAP sessions on a configured channel and synthetic sessions.
 */
class CanMonitorService final : public ServiceBase<> {
public:
    using DtCanbusProvider = std::function<const device*(uint8_t)>;

private:
    /// What a repeated OPEN has to equal to get the open session's token again.
    struct RequestKey {
        uint8_t port = 0;
        CanMonitorMode mode = CanMonitorMode::TAP;
        std::optional<CanbusType> type;
        std::optional<uint32_t> bitrate;
        std::optional<uint32_t> data_bitrate;
        std::optional<CanSyntheticParameters> synthetic;

        bool operator==(const RequestKey&) const = default;
    };

    struct OpenSession {
        RequestKey key;
        CanMonitorOpenAnswer answer;
        /// A request has carried the token since OPEN answered.
        bool is_token_used = false;
        /// Owned by the sender.
        CanMonitorSession* session = nullptr;
        std::shared_ptr<CanbusProxy> proxy;
        /// The bus the tap was set on; compared, never called, once the proxy may hold another.
        Canbus* bus = nullptr;
#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_SYNTHETIC
        std::unique_ptr<CanSyntheticSource> source;
#endif
    };

    std::shared_ptr<CanbusService> canbus_service_;
    DtCanbusProvider dt_canbus_provider_;
    CanMonitorLink link_;
    CanMonitorSender sender_;

    mutable k_mutex lock_{};
    std::vector<OpenSession> sessions_;

    [[nodiscard]] OpenSession* Find(uint8_t port);
    [[nodiscard]] OpenSession* Find(uint32_t token);
    [[nodiscard]] uint32_t NewToken() const;
    // Both require lock_.
    void EndSession(OpenSession& open, CanEndReason reason);
    void EndAll(CanEndReason reason);

    bool DoInitialize() override;
    bool DoStart() override;
    bool DoStop() override;

public:
    /**
     * @param canbus_service The configured channels; a TAP session taps one through its proxy.
     * @param dt_canbus_provider The devicetree's CAN ports (DtCanbus::Get).
     */
    CanMonitorService(
        std::shared_ptr<CanbusService> canbus_service,
        DtCanbusProvider dt_canbus_provider,
        CanMonitorLink link
    );
    ~CanMonitorService() override;

    CanMonitorService(const CanMonitorService&) = delete;
    CanMonitorService& operator=(const CanMonitorService&) = delete;
    CanMonitorService(CanMonitorService&&) = delete;
    CanMonitorService& operator=(CanMonitorService&&) = delete;

    /**
     * @brief Opens a session.
     *
     * Errors, in the order they are checked: BUSY while the service isn't running; BAD_MODE for a
     * synthetic request in a build without the source; UNKNOWN for arguments that don't hold (synthetic
     * parameters out of range, more filters than EERIE_LEAP_CAN_MONITOR_MAX_FILTERS); NOT_FOUND for a
     * port the devicetree doesn't have; BAD_MODE for a mode other than TAP, or TAP on a port the
     * configuration doesn't run; the repeated-OPEN token; NOT_LISTENING when no notification can go;
     * BUSY when the port has a session or every slot is taken; NO_MEMORY when the ring can't be had;
     * UNKNOWN when the bus refuses the tap.
     */
    std::expected<CanMonitorOpenAnswer, CanMonitorError> Open(const CanMonitorOpenRequest& request);

    /**
     * @brief Ends a session with the reason "closed"; what is left in its ring is sent first.
     * @return BAD_TOKEN for 0, NO_SESSION for a token no session holds.
     */
    std::expected<void, CanMonitorError> Close(uint32_t token);

    /** @brief A `bus` notification left; the controller routes BleService::SetBusSentHandler() here. */
    void OnNotificationSent();

    /** @brief The connection ended: every session ends (reason "disconnected", no record can go). */
    void OnDisconnected();

    /** @brief Sessions open now (the ones still draining are not). */
    [[nodiscard]] size_t GetSessionCount() const;
    /** @brief Sessions the sender still holds: the open ones and those draining before they are freed. */
    [[nodiscard]] size_t GetSenderSessionCount() {
        return sender_.GetSessionCount();
    }
    /** @brief Notifications awaiting OnNotificationSent(). */
    [[nodiscard]] uint32_t GetInFlight() const {
        return sender_.GetInFlight();
    }
};

} // namespace eerie_leap::domain::can_monitor_domain::services
