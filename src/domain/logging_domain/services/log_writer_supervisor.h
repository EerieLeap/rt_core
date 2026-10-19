#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "domain/logging_domain/configuration/logging_configuration_manager.h"
#include "domain/logging_domain/control/i_logging_control.h"
#include "domain/logging_domain/event_bus/logging_events_channel.h"
#include "domain/logging_domain/event_bus/logging_status_reason.h"
#include "domain/sensor_domain/configuration/sensors_configuration_manager.h"
#include "subsys/event_bus/dispatch_guard.h"
#include "subsys/event_bus/i_scoped_subscription.h"
#include "subsys/threading/i_service.h"
#include "subsys/threading/work_queue_task_result.h"
#include "subsys/threading/work_queue_thread.h"

#include "log_writer_service.h"

namespace eerie_leap::domain::logging_domain::services {

using eerie_leap::domain::logging_domain::control::ILoggingControl;
using eerie_leap::domain::logging_domain::event_bus::LoggingEventsChannel;
using eerie_leap::domain::logging_domain::event_bus::LoggingStatusReason;
using eerie_leap::domain::sensor_domain::configuration::SensorsConfigurationManager;
using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::event_bus::DispatchGuard;
using eerie_leap::subsys::threading::IService;

// Runs the log writer of a unit that logs. Every start, stop, restart for a stored logging
// configuration, failed-session stop, and the stop and start around a sensors restart runs on the
// configuration work queue, one at a time. Requests return at once from any thread, and the latest
// one wins. The supervisor builds each session's logger and stops a session that failed.
//
// After LoggingController::Start() every stored configuration stops logging first (L9): the stop is
// requested on the storing thread, before the store's own handler queues the restart for a logging
// configuration or the sensors restart, so neither starts the writer again. Both restarts remain for a
// store during the unit's start and for a start requested after the update's stop, which wins as the
// later request.
class LogWriterSupervisor final : public ILoggingControl {
public:
    // This unit's CDMP device ID for the log header, 0 while it has none; called at every start on
    // the configuration work queue. The writer's factory keeps it as long as the writer lives, so it
    // should hold the COM service weakly.
    using DeviceIdProvider = std::function<uint8_t()>;

    /**
     * @param config_work_queue_thread The queue SensorsController restarts processing on, so the
     *        dependent (GetDependentService()) runs in turn with the requests.
     * @param log_writer_service Initialized and stopped; its owner keeps it.
     * @param tool_id The application that wrote the log, such as "rt_daq".
     * @param device_id_provider Null: device ID 0.
     */
    LogWriterSupervisor(
        std::shared_ptr<WorkQueueThread> config_work_queue_thread,
        std::shared_ptr<LogWriterService> log_writer_service,
        std::shared_ptr<LoggingConfigurationManager> logging_configuration_manager,
        std::shared_ptr<SensorsConfigurationManager> sensors_configuration_manager,
        std::string tool_id,
        DeviceIdProvider device_id_provider = nullptr
    );

    // Withdraws the status subscription, the updated handler and the task, and waits for a run of
    // the task in progress. Leaves the writer as it is: its owner stops it. No request may come in
    // during or after it.
    ~LogWriterSupervisor() override;

    // The handlers and the task refer to it.
    LogWriterSupervisor(const LogWriterSupervisor&) = delete;
    LogWriterSupervisor& operator=(const LogWriterSupervisor&) = delete;
    LogWriterSupervisor(LogWriterSupervisor&&) = delete;
    LogWriterSupervisor& operator=(LogWriterSupervisor&&) = delete;

    /**
     * Sets the writer's logger factory, subscribes to logging_events and registers the logging
     * configuration's updated handler. Once, while the writer is stopped, before any request, and
     * after the composition root registered logging_events with a bus: without one IsActive() stays
     * false and a failed session waits for the task's next run.
     */
    bool Initialize();

    // The target is ignored: this is the writer of this unit. Which requests reach it is
    // WriterLoggingControl's routing.
    void RequestStart(uint32_t target_uid) override;
    void RequestStop(uint32_t target_uid) override;
    // The CDMP LOGGING command is a request; SUCCESS means accepted, whether or not the supervisor can
    // act on it now (a request before Initialize() is refused and logged, as for any caller).
    bool HandleLoggingCommand(bool is_start) override;
    [[nodiscard]] bool IsActive() const override;
    [[nodiscard]] LoggingStatusReason GetReason() const override;

    /**
     * What to register with SensorsController::RegisterDependentService(), after a successful
     * Initialize(). It stops the writer for a sensors restart and starts it again only while logging
     * is still wanted and did not fail. It holds the writer and the wanted state, not the
     * supervisor, so it stays harmless once the supervisor is gone.
     */
    [[nodiscard]] std::shared_ptr<IService> GetDependentService() const;

private:
    enum class Request : uint8_t {
        None,
        Start,
        Stop
    };

    // The writer and the wanted state, shared with the dependent.
    struct State;
    class DependentLogWriter;

    // Declared before task_, so the queue outlives the task.
    std::shared_ptr<WorkQueueThread> config_work_queue_thread_;
    std::shared_ptr<LoggingConfigurationManager> logging_configuration_manager_;
    std::shared_ptr<SensorsConfigurationManager> sensors_configuration_manager_;
    std::string tool_id_;
    DeviceIdProvider device_id_provider_;

    std::shared_ptr<State> state_;
    std::shared_ptr<DependentLogWriter> dependent_;
    // The handlers' way back to this; detached first by the destructor.
    std::shared_ptr<DispatchGuard> guard_;

    std::optional<WorkQueueTask<LogWriterSupervisor>> task_;
    AnySubscription status_subscription_;

    std::atomic<bool> is_initialized_ = false;
    bool is_handler_registered_ = false;
    // Set by the updated handler, taken by the task.
    std::atomic<bool> is_restart_due_ = false;
    // From the last StatusUpdated.
    std::atomic<bool> is_active_ = false;
    std::atomic<LoggingStatusReason> reason_ = LoggingStatusReason::Stopped;
    // The reason of the last failure event, for the log line of the stop; taken by every failure
    // check. Stopped: none.
    std::atomic<LoggingStatusReason> failure_reason_ = LoggingStatusReason::Stopped;

    // The task; configuration work queue only.
    WorkQueueTaskResult Process();
    void StopIfFailed();
    // Whether it opened a new session.
    bool ApplyStart();
    void ApplyStop();
    void RestartIfRunning();

    // The bus thread that carries logging_events, inside guard_.
    void OnStatusUpdated(const LoggingEventsChannel::EventMessage& event);
};

} // namespace eerie_leap::domain::logging_domain::services
