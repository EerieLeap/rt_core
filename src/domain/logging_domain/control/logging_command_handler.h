#pragma once

#include <memory>

#include "subsys/event_bus/dispatch_guard.h"
#include "domain/canbus_com_domain/services/canbus_com_service.h"

#include "i_logging_control.h"

namespace eerie_leap::domain::logging_domain::control {

using eerie_leap::subsys::event_bus::DispatchGuard;
using eerie_leap::domain::canbus_com_domain::services::CanbusComService;

// Serves the CDMP LOGGING command on a unit that logs: start or stop becomes a request, answered
// SUCCESS at once, which means accepted; the outcome follows as the shared logging status. A request
// without a payload is answered FAILURE. It runs on the CDMP work queue and never waits for the
// writer.
//
// Wiring on a unit that logs, in this order: the supervisor's Initialize(), this handler, then the
// exporter, which shares the first logging status. A command that arrives with no handler is dropped
// without an answer, and a start passed to a supervisor that is not initialized is lost, and the
// sender of a broadcast does not ask again.
//
// Replacing a handler: destroy the old one before building the new one. The destructor unsets
// whatever LOGGING handler the COM service holds, not only its own, so an old handler destroyed
// after the new one is built leaves the command unregistered.
class LoggingCommandHandler {
private:
    std::shared_ptr<CanbusComService> canbus_com_service_;
    std::shared_ptr<ILoggingControl> logging_control_;
    // The handler's way back to the control; detached first by the destructor.
    std::shared_ptr<DispatchGuard> guard_;

public:
    // Registers the handler. Throws std::invalid_argument for a null argument.
    LoggingCommandHandler(
        std::shared_ptr<CanbusComService> canbus_com_service,
        std::shared_ptr<ILoggingControl> logging_control);

    // Detaches the guard, waiting for a call in progress, then unsets the handler; a call that
    // already took the handler finishes on its own reference and is answered FAILURE. Not from the
    // handler itself.
    ~LoggingCommandHandler();

    // The registered handler refers to it.
    LoggingCommandHandler(const LoggingCommandHandler&) = delete;
    LoggingCommandHandler& operator=(const LoggingCommandHandler&) = delete;
    LoggingCommandHandler(LoggingCommandHandler&&) = delete;
    LoggingCommandHandler& operator=(LoggingCommandHandler&&) = delete;
};

} // namespace eerie_leap::domain::logging_domain::control
