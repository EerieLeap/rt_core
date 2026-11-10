#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#include "domain/can_monitor_domain/models/can_monitor_types.h"
#include "domain/can_monitor_domain/utilities/can_record.h"
#include "domain/canbus_domain/services/canbus_service.h"
#include "subsys/canbus/canbus.h"
#include "subsys/canbus/canbus_proxy.hpp"
#include "subsys/threading/service_base.h"
#include "subsys/threading/work_queue_thread.h"

#include "can_error_counters.h"
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
using eerie_leap::domain::can_monitor_domain::models::CanMonitorStats;
using eerie_leap::domain::can_monitor_domain::models::CanMonitorStatus;
using eerie_leap::domain::can_monitor_domain::models::CanSyntheticParameters;
using eerie_leap::domain::can_monitor_domain::models::CanUnitFilter;
using eerie_leap::domain::can_monitor_domain::utilities::CanEndReason;
using eerie_leap::domain::canbus_domain::services::CanbusService;
using eerie_leap::subsys::canbus::CanbusProxy;
using eerie_leap::subsys::threading::ServiceBase;
using eerie_leap::subsys::threading::WorkQueueTask;
using eerie_leap::subsys::threading::WorkQueueThread;

/**
 * @brief The CAN monitor's sessions: OPEN, CLOSE, FILTER, STATS and STATUS, the leases, and the sender
 * that streams them.
 *
 * - Initialize() starts the sender and the domain's work queue; the sender keeps running until the service
 *   is destroyed so that sessions ended by Stop() still drain. Start() lets OPEN in; Stop() ends every
 *   session with the reason "configuration stored" (§3.5), which is how
 *   CanbusController::RegisterDependentService() uses it before a new CAN configuration is applied.
 * - At most EERIE_LEAP_CAN_MONITOR_MAX_SESSIONS sessions, at most one a port (255 for the synthetic one),
 *   all under one k_mutex that the tap never takes. The lock is taken before the sender's.
 * - Sessions: TAP on a channel the stored configuration runs; LISTEN_ONLY and ACTIVE on a port it doesn't,
 *   on a Canbus the session builds and frees; a synthetic one with no bus.
 * - Every request that carries a valid token renews the session's lease (EERIE_LEAP_CAN_MONITOR_LEASE_MS)
 *   and marks the token used. A lapsed lease ends the session with the reason LEASE.
 * - Open() answers the same token to a repeated OPEN (same port, mode, type, bitrates) while no later
 *   request has carried the token: a client that lost the answer gets its session, not BUSY.
 *
 * The domain's work queue runs the lease and the disconnect, never the system work queue: ending a session
 * that owns a bus joins the bus's thread, which can take seconds while it detects a bitrate.
 */
class CanMonitorService final : public ServiceBase<> {
public:
    using DtCanbusProvider = std::function<const device*(uint8_t)>;

    /// STATUS asks the provider for the ports 0 to MAX_PORTS - 1.
    static constexpr uint8_t MAX_PORTS = 8;

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
        /// k_uptime_get() at which the session ends unless a request renews it.
        int64_t lease_deadline_ms = 0;
        /// Owned by the sender.
        CanMonitorSession* session = nullptr;
        std::shared_ptr<CanbusProxy> proxy;
        /// The controller of the port; none for a synthetic session.
        const device* controller = nullptr;
        /// The bus the tap was set on: the channel's, or the session's own. Compared with the proxy's, never
        /// called, once the proxy may hold another.
        Canbus* bus = nullptr;
        /// The bus a LISTEN_ONLY or ACTIVE session built on its port.
        std::unique_ptr<Canbus> own_bus;
#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_SYNTHETIC
        std::unique_ptr<CanSyntheticSource> source;
#endif
    };

    std::shared_ptr<CanbusService> canbus_service_;
    DtCanbusProvider dt_canbus_provider_;
    CanMonitorLink link_;
    CanErrorCountersFactory counters_factory_;
    CanMonitorSender sender_;

    mutable k_mutex lock_{};
    std::vector<OpenSession> sessions_;

    // Created by Initialize(). The tasks refer to this and are cancelled first in the destructor.
    std::unique_ptr<WorkQueueThread> work_queue_;
    std::optional<WorkQueueTask<CanMonitorService>> lease_task_;
    std::optional<WorkQueueTask<CanMonitorService>> disconnect_task_;

    [[nodiscard]] OpenSession* Find(uint8_t port);
    [[nodiscard]] OpenSession* Find(uint32_t token);
    [[nodiscard]] const OpenSession* FindConst(uint8_t port) const;
    [[nodiscard]] uint32_t NewToken() const;
    [[nodiscard]] std::shared_ptr<ICanErrorCounters> MakeCounters(const device* controller) const;
    /// The session of @p token with its lease renewed, or why there is none. Requires lock_.
    [[nodiscard]] std::expected<OpenSession*, CanMonitorError> Touch(uint32_t token);
    // The following require lock_.
    void EndSession(OpenSession& open, CanEndReason reason);
    void EndAll(CanEndReason reason);
    void ScheduleHousekeeping();
    void EndExpired();
    void EndOnDisconnect();

    bool DoInitialize() override;
    bool DoStart() override;
    bool DoStop() override;

public:
    /**
     * @param canbus_service The configured channels; a TAP session taps one through its proxy.
     * @param dt_canbus_provider The devicetree's CAN ports (DtCanbus::Get).
     * @param counters_factory Where sessions get the controller's error counts; empty reads Zephyr's
     * (CONFIG_CAN_STATS), tests stub it.
     */
    CanMonitorService(
        std::shared_ptr<CanbusService> canbus_service,
        DtCanbusProvider dt_canbus_provider,
        CanMonitorLink link,
        CanErrorCountersFactory counters_factory = nullptr
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
     * port the devicetree doesn't have; BAD_MODE for a mode nobody knows, TAP on a port the configuration
     * doesn't run, LISTEN_ONLY or ACTIVE on one it runs, or CAN FD on a controller without it; BAD_BITRATE
     * for a bitrate Canbus::IsBitrateSupported() refuses; the repeated-OPEN token; NOT_LISTENING when no
     * notification can go; BUSY when the port has a session or every slot is taken; NO_MEMORY when the
     * ring (or the changed-bytes table) can't be had; BAD_BITRATE or UNKNOWN when the port's own bus won't
     * start (BAD_BITRATE with a bitrate given); UNKNOWN when a bus refuses the tap.
     *
     * A session on its own bus detects the bitrate when OPEN gives none (the answer says 0, the first state
     * record carries the bitrate). `delta` is ignored in a build without EERIE_LEAP_CAN_MONITOR_DELTA.
     */
    std::expected<CanMonitorOpenAnswer, CanMonitorError> Open(const CanMonitorOpenRequest& request);

    /**
     * @brief Ends a session with the reason "closed"; what is left in its ring is sent first.
     * @return BAD_TOKEN for 0, NO_SESSION for a token no session holds.
     */
    std::expected<void, CanMonitorError> Close(uint32_t token);

    /**
     * @brief Replaces the unit filters of a session; an empty list passes every frame.
     * @return BAD_TOKEN, NO_SESSION; UNKNOWN for more filters than EERIE_LEAP_CAN_MONITOR_MAX_FILTERS.
     */
    std::expected<void, CanMonitorError> SetFilters(uint32_t token, std::span<const CanUnitFilter> filters);

    /** @brief Totals since OPEN, and the controller's state now. @return BAD_TOKEN, NO_SESSION. */
    std::expected<CanMonitorStats, CanMonitorError> GetStats(uint32_t token);

    /** @brief The ports the devicetree has and what the link and the build allow. */
    [[nodiscard]] CanMonitorStatus GetStatus() const;

    /** @brief A `bus` notification left; the controller routes BleService::SetBusSentHandler() here. */
    void OnNotificationSent();

    /**
     * @brief The connection ended; the controller routes Ble's disconnected handler here.
     *
     * Returns at once: every session ends on the domain's work queue with the reason "disconnected", and no
     * record can go. What is in flight is forgotten right away.
     */
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
    /**
     * @brief The bus a LISTEN_ONLY or ACTIVE session built, for tests to look at its configuration.
     * @return nullptr for another kind of session or an unknown token. Valid until the session ends.
     */
    [[nodiscard]] Canbus* GetOwnBus(uint32_t token);
};

} // namespace eerie_leap::domain::can_monitor_domain::services
