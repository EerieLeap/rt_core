#pragma once

#include <cstdint>
#include <functional>

namespace eerie_leap::domain::logging_domain::control {

// Reports a logging unit that restarted and is back, idle, so that RemoteLoggingControl can send a
// wanted start again. Tests give it a fake.
class ILoggingRestartMonitor {
public:
    // The unit's CDMP device ID, for the log.
    using RestartHandler = std::function<void(uint8_t device_id)>;

    virtual ~ILoggingRestartMonitor() = default;

    // Called once per restart of a logging unit, when the unit shares an inactive Stopped status
    // again, on the thread the implementation names. Replaces the handler; null removes it. A call
    // already under way finishes with the handler it started with, so an owner detaches its own
    // guard first. One handler, one owner. Reports while no handler is set are dropped.
    //
    // The handler must not block or throw. Setting one allocates, so it can throw
    // std::bad_alloc; removing one does not.
    virtual void SetRestartHandler(RestartHandler handler) = 0;
};

} // namespace eerie_leap::domain::logging_domain::control
