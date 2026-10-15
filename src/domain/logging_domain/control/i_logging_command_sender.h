#pragma once

namespace eerie_leap::domain::logging_domain::control {

// Sends the CDMP LOGGING command to the units that log. RemoteLoggingControl calls it on the
// configuration work queue; tests give it a fake.
class ILoggingCommandSender {
public:
    virtual ~ILoggingCommandSender() = default;

    // Broadcasts start or stop and returns without waiting for an answer; the outcome arrives as the
    // logging status the units share.
    virtual void SendLoggingCommand(bool is_start) = 0;
};

} // namespace eerie_leap::domain::logging_domain::control
