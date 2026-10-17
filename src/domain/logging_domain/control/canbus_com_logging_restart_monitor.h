#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <memory>
#include <optional>

#include <zephyr/kernel.h>

#include "subsys/cdmp/models/cdmp_device_info.h"
#include "subsys/cdmp/models/cdmp_message.h"
#include "subsys/cdmp/services/cdmp_network_service.h"

#include "domain/canbus_com_domain/services/canbus_com_service.h"
#include "domain/canbus_com_domain/services/i_network_state_transport.h"
#include "domain/canbus_com_domain/states/canbus_com_state_data.h"

#include "i_logging_restart_monitor.h"

namespace eerie_leap::domain::logging_domain::control {

using eerie_leap::subsys::cdmp::models::CdmpDeviceInfo;
using eerie_leap::subsys::cdmp::models::CdmpHeartbeatMessage;
using eerie_leap::subsys::cdmp::services::CdmpDeviceEvent;
using eerie_leap::domain::canbus_com_domain::services::CanbusComService;
using eerie_leap::domain::canbus_com_domain::services::INetworkStateTransport;
using eerie_leap::domain::canbus_com_domain::states::RemoteStateRecord;

// Tells RemoteLoggingControl that a logging unit restarted, or joined while another unit reports, and
// shares an idle status again, from CDMP's device events, heartbeats and the units' LOGGING_STATUS
// copies.
//
// Why the status alone is not enough: a unit that restarts within the heartbeat timeout (3 x 3 s)
// keeps its copies, stale, and the importer shows only a change of the combined status. A unit that
// was idle before the restart shows no change at all, and one that logged goes from active straight
// to Stopped, never through Unavailable. With another unit reporting, the combined status does not
// pass through Unavailable when a unit comes back either. So the unit is followed on its own copy.
//
// A unit is armed when it joins (CdmpDeviceEvent::JOINED, after the heartbeat timeout or for the
// first time) or claims its ID again with its UID (RESTARTED) and
// - it is a LOGGER, which may share a status, or
// - this unit holds a LOGGING_STATUS copy of it: the record is what makes a unit a logging unit.
// A unit that joined has no copy yet, so its type is all there is to go by. With no copy at all the
// status goes through Unavailable when the unit reports, which the status rule covers; with another
// unit's copy it does not, and the monitor is the only signal.
//
// It is resolved at the first check that finds the unit's copy current again, which is
// - a LOGGING_STATUS state handler call, when the new record changed the copy's value (a unit that
//   logged is Stopped now): at once; and
// - a heartbeat of the unit, when the new record equals the stale copy (a unit that was idle, or
//   that logs again) or the copy was confirmed by its digest: at most one heartbeat interval (3 s)
//   after the record.
// Inactive Stopped is reported once. An active status (the unit started by itself) or a failure
// consumes the restart without a report. What the unit shares meanwhile is seen only if it changes
// the value: a unit that started by itself with a record equal to the stale copy, and stopped again
// before its next heartbeat, is reported, and the wanted start overrides that stop.
//
// Why a current copy means the unit can take the LOGGING command: the writer role builds the
// command handler before the exporter that shares the record.
//
// It forgets a unit when it leaves the table (LEFT), and nowhere else. While no unit has a copy at
// all, the combined status is Unavailable and RemoteLoggingControl's status rule sends one start
// when it returns to Stopped, so the units stay armed through that spell. (This unit's own COM
// restart is one: it drops the copies and the table without an event.) The state handler call that
// ends the spell reads the combined status, as the importer does:
// - Stopped: the rule sends, and its broadcast reaches every unit that has a copy, so those units are
//   cleared without a report. A unit with no copy yet stays armed: it may still be booting, with no
//   LOGGING handler to take that broadcast.
// - active or a failure: the rule does not send, and a unit that is back and Stopped is reported.
// A heartbeat resolves nothing during the spell, since a record can be applied and its heartbeat
// handled before the call that ends it.
//
// It tells a unit that rebooted from one whose COM service restarted (a CAN configuration applied on
// it) by nothing: both claim their ID again with their UID. A COM restart keeps the unit's record
// versions and a reboot usually starts them again at 1, which would tell them apart except for a
// status record that was not stamped again since boot, so the monitor reports both.
//
// The state, device event and heartbeat handlers all run on the CDMP work queue, one at a time, and
// so do the reports. The monitor relies on a stable CDMP UID (hwinfo): a unit that claims another ID
// or UID after a reboot is a new unit, not a restart.
class CanbusComLoggingRestartMonitor final : public ILoggingRestartMonitor {
public:
    // Registers its handlers with the COM service. Throws std::invalid_argument for a null service,
    // and std::runtime_error when the service refuses a handler, with nothing left registered.
    //
    // @p has_status_rule: whether the control it reports to follows the logging status and sends a
    // start when the status returns from Unavailable to Stopped (RemoteLoggingControl::Mode::Remote).
    // A control that does not (Mode::Peer) has no rule to leave a unit to, so the monitor reports every
    // unit itself, in the spell without any copy as well.
    explicit CanbusComLoggingRestartMonitor(
        std::shared_ptr<CanbusComService> canbus_com_service,
        bool has_status_rule = true);

    // Unregisters its handlers, waiting for one in progress. Not on the CDMP work queue.
    ~CanbusComLoggingRestartMonitor() override;

    // The handlers refer to it.
    CanbusComLoggingRestartMonitor(const CanbusComLoggingRestartMonitor&) = delete;
    CanbusComLoggingRestartMonitor& operator=(const CanbusComLoggingRestartMonitor&) = delete;
    CanbusComLoggingRestartMonitor(CanbusComLoggingRestartMonitor&&) = delete;
    CanbusComLoggingRestartMonitor& operator=(CanbusComLoggingRestartMonitor&&) = delete;

    void SetRestartHandler(RestartHandler handler) override;

private:
    // Holds the service strongly, like the command sender: the controller that builds both outlives them.
    std::shared_ptr<CanbusComService> canbus_com_service_;
    const bool has_status_rule_;
    int state_handler_id_ = -1;
    int device_event_handler_id_ = -1;
    int heartbeat_handler_id_ = -1;

    // The slot: SetRestartHandler() swaps it under the lock, a report copies it under the lock and
    // calls it outside, so a handler replaced during its call finishes that call.
    mutable k_mutex handler_lock_;
    std::shared_ptr<const RestartHandler> handler_;

    // The CDMP work queue only, as in NetworkStateImporter. The records are a member so that they
    // stay off the work queue's stack.
    std::bitset<256> restarted_;
    // Whether the last state handler call found no LOGGING_STATUS copy at all, which is the combined
    // status Unavailable in that same call.
    bool is_status_empty_ = false;
    std::array<RemoteStateRecord, INetworkStateTransport::MAX_REMOTE_RECORDS> records_{};

    // Unregisters whatever is registered. The destructor, and the constructor that fails.
    void Unregister();

    // The three handlers. They run inside the network and state services' notification loops. The
    // network service's loops catch nothing, so a throw there skips the handlers after this one; the
    // state service catches and logs. None of them throws.
    void OnDeviceEvent(CdmpDeviceEvent event, const CdmpDeviceInfo& device);
    // Whether a unit that joined or restarted is a unit to follow: a logger, or one with a copy.
    // Reads records_.
    [[nodiscard]] bool IsLoggingUnit(const CdmpDeviceInfo& device);
    void OnHeartbeat(const CdmpHeartbeatMessage& heartbeat);
    void OnStatesChanged();

    // Resolves the armed units among the first @p count records_: inactive Stopped is reported,
    // anything else is consumed, and a unit without a current copy stays armed. It looks at the unit
    // @p device_id (a heartbeat), or at every unit (a state handler call). @p is_status_returning:
    // this state handler call ends a spell without any copy, so the status rule sends for what it
    // can, and the units it covers are only cleared.
    void Resolve(size_t count, std::optional<uint8_t> device_id, bool is_status_returning);

    // Whether the combined status of the first @p count records_ is inactive Stopped, as
    // LoggingNetworkState::Combine() makes it: no unit logs and none failed.
    [[nodiscard]] bool IsCombinedStatusStopped(size_t count) const;

    void Report(uint8_t device_id);
};

} // namespace eerie_leap::domain::logging_domain::control
