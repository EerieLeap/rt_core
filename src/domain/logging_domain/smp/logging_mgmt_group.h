#pragma once

#include <cstdint>
#include <memory>

#include "domain/logging_domain/control/i_logging_control.h"
#include "domain/logging_domain/models/log_session_info.h"
#include "subsys/smp/smp_group.h"

namespace eerie_leap::domain::logging_domain::smp {

using eerie_leap::domain::logging_domain::control::ILoggingControl;
using eerie_leap::domain::logging_domain::models::LogSessionProvider;
using eerie_leap::subsys::smp::SmpGroup;

/**
 * @brief SMP `logging` group: the unit's logging status, and start and stop, for a client that reaches
 * the unit over SMP (Bluetooth, or routed through an access point) and so cannot use the CDMP
 * LOGGING command.
 *
 * Served by both roles. STATUS answers what the control last saw on logging_events: this unit's own
 * status with a log writer, the combined status the units share (imported) without one. START and STOP
 * make the control's request, exactly as a trigger does. The handlers only read atomics and store a
 * request: no allocation, no exceptions, no blocking, so they are safe on the MCUmgr work queue.
 */
class LoggingMgmtGroup : public SmpGroup<LoggingMgmtGroup> {
public:
    /** @brief Command IDs. */
    enum class Command : uint8_t {
        /// Read: `{active, reason, since_ms?, file?, bytes?}`. `reason` is a LoggingStatusReason, 0
        /// while active. `since_ms` (the session's start, Unix ms UTC) only while this unit's writer
        /// runs a session; `file` (asset name in the `logs` kind) and `bytes` only with a file open.
        STATUS = 0,
        /// Write `{uid?}`: `{}`. `uid` is the CDMP UID of the unit to start; absent or 0 is every unit.
        START = 1,
        /// Write `{uid?}`: `{}`. As START.
        STOP = 2,
    };

private:
    static const mgmt_handler HANDLERS[];

    // The group answers no group errors of its own: a bad request is MGMT_ERR_EINVAL.
    static int TranslateError(uint16_t error);

    std::shared_ptr<ILoggingControl> control_;
    LogSessionProvider session_provider_;

    int HandleStatus(smp_streamer* ctxt);
    int HandleStart(smp_streamer* ctxt);
    int HandleStop(smp_streamer* ctxt);

    /** @brief Decodes the optional `uid`; false for a value that is not an unsigned 32-bit integer. */
    static bool DecodeUid(smp_streamer* ctxt, uint32_t& uid);

public:
    /**
     * @param control The unit's logging control, required.
     * @param session_provider The log writer's session; empty on a unit without a writer, where STATUS
     *        answers `active` and `reason` alone.
     */
    explicit LoggingMgmtGroup(std::shared_ptr<ILoggingControl> control, LogSessionProvider session_provider = {});
    ~LoggingMgmtGroup();

    LoggingMgmtGroup(const LoggingMgmtGroup&) = delete;
    LoggingMgmtGroup& operator=(const LoggingMgmtGroup&) = delete;
};

} // namespace eerie_leap::domain::logging_domain::smp
