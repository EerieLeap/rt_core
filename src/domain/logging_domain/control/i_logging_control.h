#pragma once

namespace eerie_leap::domain::logging_domain::control {

// Starts and stops logging, on this unit or on the units that log, and tells whether it runs.
class ILoggingControl {
public:
    virtual ~ILoggingControl() = default;

    // Records the request and returns; callable from any thread. The latest request wins. A change of
    // state arrives as StatusUpdated on logging_events. A request that changes nothing publishes
    // nothing: a stop while stopped, or a start that fails (logged on the unit that logs). So a
    // requester cannot tell a refusal from a request not yet served.
    virtual void RequestStart() = 0;
    virtual void RequestStop() = 0;

    // Whether logging runs, as logging_events last reported it: for the shell's status and for
    // tests. A trigger never reads it, its bound value already says what it wants.
    [[nodiscard]] virtual bool IsActive() const = 0;
};

} // namespace eerie_leap::domain::logging_domain::control
