#include <optional>
#include <stdexcept>
#include <utility>

#include <zephyr/logging/log.h>

#include "domain/canbus_com_domain/commands/canbus_com_command_code.h"
#include "domain/canbus_com_domain/commands/canbus_com_logging_command.h"

#include "logging_command_handler.h"

namespace eerie_leap::domain::logging_domain::control {

using eerie_leap::domain::canbus_com_domain::commands::CanbusComCommandCode;
using eerie_leap::domain::canbus_com_domain::commands::CanbusComLoggingCommand;

LOG_MODULE_REGISTER(logging_command_handler_logger);

LoggingCommandHandler::LoggingCommandHandler(
    std::shared_ptr<CanbusComService> canbus_com_service,
    std::shared_ptr<ILoggingControl> logging_control
) :
    canbus_com_service_(std::move(canbus_com_service)),
    logging_control_(std::move(logging_control)),
    guard_(std::make_shared<DispatchGuard>(this)) {

    if(canbus_com_service_ == nullptr || logging_control_ == nullptr)
        throw std::invalid_argument("The logging command handler needs the COM service and a logging control.");

    // The raw pointer is valid while the guard is attached: this object holds the control until
    // after Detach(). The closure keeps nothing alive, so the control is never destroyed on the CDMP
    // work queue.
    canbus_com_service_->SetCommandHandler<CanbusComLoggingCommand>(
        CanbusComCommandCode::LOGGING,
        [guard = guard_, control = logging_control_.get()](std::optional<CanbusComLoggingCommand> command) {
            // A request without a payload names neither start nor stop.
            if(!command.has_value())
                return false;

            bool is_accepted = false;
            guard->Dispatch([&] {
                LOG_DBG("Logging %s command received.", command->IsStart() ? "start" : "stop");

                is_accepted = control->HandleLoggingCommand(command->IsStart());
            });

            // False only while the handler is being destroyed.
            return is_accepted;
        }
    );
}

LoggingCommandHandler::~LoggingCommandHandler() {
    // A call in its body is waited for; one that comes later answers FAILURE.
    guard_->Detach();

    canbus_com_service_->UnsetCommandHandler(CanbusComCommandCode::LOGGING);
}

} // namespace eerie_leap::domain::logging_domain::control
