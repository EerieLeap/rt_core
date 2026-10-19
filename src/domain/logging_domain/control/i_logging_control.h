#pragma once

#include <cstdint>

#include "domain/logging_domain/event_bus/logging_status_reason.h"

namespace eerie_leap::domain::logging_domain::control {

using eerie_leap::domain::logging_domain::event_bus::LoggingStatusReason;

// Starts and stops logging, on this unit or on the units that log, and tells whether it runs.
class ILoggingControl {
public:
    virtual ~ILoggingControl() = default;

    // Records the request and returns; callable from any thread. The latest request wins. A change of
    // state arrives as StatusUpdated on logging_events. A request that changes nothing publishes
    // nothing: a stop while stopped, or a start that fails (logged on the unit that logs). So a
    // requester cannot tell a refusal from a request not yet served.
    //
    // @p target_uid is the CDMP UID of the unit to start or stop; ALL_UNITS is every unit that can log.
    // A unit with a log writer acts on its own state for ALL_UNITS or its own UID (WriterLoggingControl
    // routes the rest to the network); the control of a unit without a writer sends everything out.
    static constexpr uint32_t ALL_UNITS = 0;
    virtual void RequestStart(uint32_t target_uid) = 0;
    virtual void RequestStop(uint32_t target_uid) = 0;

    // The CDMP LOGGING command another unit sent (LoggingCommandHandler, on the CDMP work queue):
    // whether this unit accepts it, answered SUCCESS, else FAILURE. A unit that logs takes it as a
    // request. A unit without a writer serves only a stop, which drops its wanted start (L9). Never
    // blocks.
    virtual bool HandleLoggingCommand(bool is_start) = 0;

    // Whether logging runs, as logging_events last reported it: for the shell's status and for
    // tests. A trigger never reads it, its bound value already says what it wants.
    [[nodiscard]] virtual bool IsActive() const = 0;

    // Why logging is not active, as logging_events last reported it, like IsActive(): Stopped while
    // active or before the first status. In the remote role it is the combined imported status
    // (Unavailable when no unit reports one). Lock-free, callable from the MCUmgr work queue.
    [[nodiscard]] virtual LoggingStatusReason GetReason() const = 0;
};

} // namespace eerie_leap::domain::logging_domain::control
