#pragma once

#include <cstdint>

#include "subsys/cdmp/utilities/constants.h"

namespace eerie_leap::domain::logging_domain::control {

// Sends the CDMP LOGGING command to the units that log, or to one of them. RemoteLoggingControl calls it on the
// configuration work queue; tests give it a fake.
class ILoggingCommandSender {
public:
    virtual ~ILoggingCommandSender() = default;

    // The CDMP device ID that addresses every unit.
    static constexpr uint8_t kBroadcast = eerie_leap::subsys::cdmp::utilities::CdmpConstants::COMMAND_BROADCAST_ID;

    // Sends start or stop to @p device_id (kBroadcast or one CDMP device ID) and returns without waiting
    // for an answer; a change of state arrives as the logging status the units share.
    virtual void SendLoggingCommand(bool is_start, uint8_t device_id) = 0;
};

} // namespace eerie_leap::domain::logging_domain::control
