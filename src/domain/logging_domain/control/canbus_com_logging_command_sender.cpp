#include <stdexcept>
#include <utility>

#include <zephyr/logging/log.h>

#include "domain/canbus_com_domain/commands/canbus_com_logging_command.h"

#include "canbus_com_logging_command_sender.h"

namespace eerie_leap::domain::logging_domain::control {

using eerie_leap::domain::canbus_com_domain::commands::CanbusComLoggingCommand;

LOG_MODULE_REGISTER(canbus_com_logging_command_sender_logger);

CanbusComLoggingCommandSender::CanbusComLoggingCommandSender(std::shared_ptr<CanbusComService> canbus_com_service)
    : canbus_com_service_(std::move(canbus_com_service)) {

    if(canbus_com_service_ == nullptr)
        throw std::invalid_argument("The logging command sender needs the COM service.");
}

void CanbusComLoggingCommandSender::SendLoggingCommand(bool is_start) {
    // The command is broadcast, so every logging unit follows it, and the status they share, not the
    // response, tells whether they did. The transaction may complete or time out after this sender is
    // gone, so the callback captures only the flag.
    CanbusComLoggingCommand command(is_start);
    canbus_com_service_->SendCommand(
        command,
        [is_start](bool success) {
            if(!success)
                LOG_WRN("Logging %s command failed.", is_start ? "start" : "stop");
        });
}

} // namespace eerie_leap::domain::logging_domain::control
