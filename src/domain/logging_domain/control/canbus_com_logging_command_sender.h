#pragma once

#include <memory>

#include "domain/canbus_com_domain/services/canbus_com_service.h"

#include "i_logging_command_sender.h"

namespace eerie_leap::domain::logging_domain::control {

using eerie_leap::domain::canbus_com_domain::services::CanbusComService;

// The LOGGING broadcast over the COM service. Holds the service strongly: the controller that builds
// both outlives them.
class CanbusComLoggingCommandSender final : public ILoggingCommandSender {
private:
    std::shared_ptr<CanbusComService> canbus_com_service_;

public:
    // Throws std::invalid_argument for a null service.
    explicit CanbusComLoggingCommandSender(std::shared_ptr<CanbusComService> canbus_com_service);

    // Starts the CDMP transaction and sends the CAN frame on the calling thread, so call it from the
    // configuration work queue, not from a bus thread. A response that fails, or none in time (no
    // logging unit online), is logged by the callback, which holds nothing that can die.
    void SendLoggingCommand(bool is_start, uint8_t device_id) override;
};

} // namespace eerie_leap::domain::logging_domain::control
