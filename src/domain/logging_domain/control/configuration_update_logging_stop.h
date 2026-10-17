#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

#include "subsys/event_bus/dispatch_guard.h"
#include "subsys/threading/work_queue_task.h"
#include "subsys/threading/work_queue_task_result.h"
#include "subsys/threading/work_queue_thread.h"
#include "domain/configuration_domain/services/configuration_service.h"

#include "i_logging_command_sender.h"
#include "i_logging_control.h"

namespace eerie_leap::domain::logging_domain::control {

using eerie_leap::subsys::event_bus::DispatchGuard;
using eerie_leap::subsys::threading::WorkQueueTask;
using eerie_leap::subsys::threading::WorkQueueTaskResult;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::domain::configuration_domain::services::ConfigurationService;

// Stops logging on every unit when a configuration is stored on this one after it has started (L9):
// the unit's own control stops for all units (a writer stops for good and its network half sends the
// LOGGING stop; a remote control drops its wanted start and sends it), and with a sender, for a unit
// without a control, the LOGGING stop is broadcast, so every other unit that logs stops too. The stores are those the ConfigurationService reports: an SMP write of any type
// over any transport, a manager's Update() or Adopt(). A rejected write, UpdateOwnSetting() and
// anything stored before Start() (the unit's boot: defaults, test hooks) are not updates.
//
// One per ConfigurationService: it takes the service's stored handler. LoggingController builds it in
// both roles; a unit without a logging controller builds it with a sender only.
//
// The report runs on the storing thread, before the store's own handler queues its work (a sensors
// restart, the writer's restart for a logging configuration, a CAN reconfiguration), so the stop is
// first in line: the writer's stop runs before any restart and nothing starts it again, and the
// broadcast leaves before the COM service stops for a CAN reconfiguration.
class ConfigurationUpdateLoggingStop {
public:
    using Type = ConfigurationService::Type;
    // Runs first on every update, on the storing thread, before the stop is requested; may throw
    // (logged, the stop still follows). LoggingController's makes its triggers take a baseline.
    using UpdateHandler = std::function<void(Type type)>;

    /**
     * @param logging_control The unit's control, null without one; RequestStop() on the storing thread.
     * @param stop_sender Broadcasts the LOGGING stop from the configuration work queue; null where the
     *        control sends it (RequestStop(kAllUnits) of both roles' controls) or the unit has no COM
     *        service. It serves a unit without a logging controller.
     * Throws std::invalid_argument for a null service or queue.
     */
    ConfigurationUpdateLoggingStop(
        std::shared_ptr<ConfigurationService> configuration_service,
        std::shared_ptr<WorkQueueThread> config_work_queue_thread,
        std::shared_ptr<ILoggingControl> logging_control,
        std::shared_ptr<ILoggingCommandSender> stop_sender,
        UpdateHandler on_update = nullptr);

    // Detaches the guard (waits for a report in its body), clears the stored handler if Start() set it,
    // cancels the broadcast and waits for one in progress. Not from inside a report, nor on the
    // configuration work queue while a broadcast it must wait for runs there.
    ~ConfigurationUpdateLoggingStop();

    // The stored handler and the task refer to it.
    ConfigurationUpdateLoggingStop(const ConfigurationUpdateLoggingStop&) = delete;
    ConfigurationUpdateLoggingStop& operator=(const ConfigurationUpdateLoggingStop&) = delete;
    ConfigurationUpdateLoggingStop(ConfigurationUpdateLoggingStop&&) = delete;
    ConfigurationUpdateLoggingStop& operator=(ConfigurationUpdateLoggingStop&&) = delete;

    // Creates the broadcast task (only with a sender). False when the queue does not run. Once.
    bool Initialize();
    // From now on a stored configuration stops logging. After Initialize(); once, from the thread that
    // built it: once the unit has finished starting, before anything writes configurations from
    // outside. Sets the service's stored handler. False before Initialize() and on a second call.
    bool Start();
    [[nodiscard]] bool IsStarted() const;

private:
    std::shared_ptr<ConfigurationService> configuration_service_;
    // Declared before broadcast_task_, so the queue outlives the task.
    std::shared_ptr<WorkQueueThread> config_work_queue_thread_;
    std::shared_ptr<ILoggingControl> logging_control_;
    std::shared_ptr<ILoggingCommandSender> stop_sender_;
    UpdateHandler on_update_;
    // The stored handler's way back to this; detached first by the destructor.
    std::shared_ptr<DispatchGuard> guard_;
    std::optional<WorkQueueTask<ConfigurationUpdateLoggingStop>> broadcast_task_;
    bool is_initialized_ = false;
    std::atomic<bool> is_started_ = false;

    // The storing thread, inside guard_.
    void OnStored(Type type);
    // Configuration work queue only.
    WorkQueueTaskResult Broadcast();
};

} // namespace eerie_leap::domain::logging_domain::control
