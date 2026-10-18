#include <exception>
#include <stdexcept>
#include <utility>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "configuration_update_logging_stop.h"

namespace eerie_leap::domain::logging_domain::control {

LOG_MODULE_REGISTER(configuration_update_logging_stop_logger);

ConfigurationUpdateLoggingStop::ConfigurationUpdateLoggingStop(
    std::shared_ptr<ConfigurationService> configuration_service,
    std::shared_ptr<WorkQueueThread> config_work_queue_thread,
    std::shared_ptr<ILoggingControl> logging_control,
    std::shared_ptr<ILoggingCommandSender> stop_sender,
    UpdateHandler on_update
) :
    configuration_service_(std::move(configuration_service)),
    config_work_queue_thread_(std::move(config_work_queue_thread)),
    logging_control_(std::move(logging_control)),
    stop_sender_(std::move(stop_sender)),
    on_update_(std::move(on_update)),
    guard_(std::make_shared<DispatchGuard>(this)) {

    if(configuration_service_ == nullptr || config_work_queue_thread_ == nullptr)
        throw std::invalid_argument(
            "The configuration update stop needs the configuration service and the configuration work queue."
        );
}

ConfigurationUpdateLoggingStop::~ConfigurationUpdateLoggingStop() {
    // A report the service's handler already started becomes a no-op, and one in its body is waited
    // for; neither requests or queues anything afterwards.
    guard_->Detach();

    if(is_started_)
        configuration_service_->RegisterConfigurationStoredHandler(nullptr);

    // Drops a pending broadcast and waits for one in progress.
    if(broadcast_task_.has_value())
        broadcast_task_->Cancel();
}

bool ConfigurationUpdateLoggingStop::Initialize() {
    if(is_initialized_)
        return true;

    if(stop_sender_ != nullptr) {
        try {
            broadcast_task_ = config_work_queue_thread_->CreateTask(
                [](ConfigurationUpdateLoggingStop* update_stop) {
                    return update_stop->Broadcast();
                },
                this
            );
        } catch(const std::exception& e) {
            // The queue is not running.
            LOG_ERR("Failed to create the configuration update stop's task: %s", e.what());
            return false;
        }
    }

    is_initialized_ = true;

    return true;
}

bool ConfigurationUpdateLoggingStop::Start() {
    if(!is_initialized_) {
        LOG_ERR("The configuration update stop is started after Initialize().");
        return false;
    }

    if(is_started_) {
        LOG_ERR("The configuration update stop is started once.");
        return false;
    }

    try {
        configuration_service_->RegisterConfigurationStoredHandler([guard = guard_, this](Type type) {
            guard->Dispatch([&] {
                OnStored(type);
            });
        });
    } catch(const std::exception& e) {
        LOG_ERR("Failed to register the configuration update stop: %s", e.what());
        return false;
    }

    is_started_ = true;

    return true;
}

bool ConfigurationUpdateLoggingStop::IsStarted() const {
    return is_started_;
}

// On the storing thread, before the store's own handler queues its work, so the stop is first in line.
void ConfigurationUpdateLoggingStop::OnStored(Type type) {
    LOG_WRN(
        "Configuration type %u stored: logging stops on every unit until it is started again.",
        static_cast<unsigned>(type)
    );

    if(on_update_) {
        try {
            on_update_(type);
        } catch(const std::exception& e) {
            LOG_ERR("The configuration update handler failed: %s", e.what());
        } catch(...) {
            LOG_ERR("The configuration update handler failed.");
        }
    }

    if(logging_control_ != nullptr)
        logging_control_->RequestStop(ILoggingControl::kAllUnits);

    // Several stores before the task runs send one stop.
    if(broadcast_task_.has_value())
        broadcast_task_->Reschedule(K_NO_WAIT);
}

WorkQueueTaskResult ConfigurationUpdateLoggingStop::Broadcast() {
    stop_sender_->SendLoggingCommand(false, ILoggingCommandSender::kBroadcast);

    return {};
}

} // namespace eerie_leap::domain::logging_domain::control
