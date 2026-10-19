#include <exception>
#include <string>
#include <utility>
#include <variant>

#include <zephyr/logging/log.h>

#include "domain/logging_domain/event_bus/logging_event_type.h"
#include "domain/logging_domain/event_bus/logging_payload_type.h"
#include "domain/logging_domain/event_bus/logging_status_reason.h"
#include "domain/logging_domain/loggers/logger_metadata.h"
#include "domain/logging_domain/loggers/mdf4_logger_sensor_reading.h"
#include "domain/system_domain/models/system_configuration.h"
#include "subsys/event_bus/event_channel.h"

#include "log_writer_supervisor.h"

namespace eerie_leap::domain::logging_domain::services {

using eerie_leap::domain::logging_domain::event_bus::GetReasonName;
using eerie_leap::domain::logging_domain::event_bus::LoggingEventType;
using eerie_leap::domain::logging_domain::event_bus::LoggingPayloadType;
using eerie_leap::domain::logging_domain::loggers::LoggerMetadata;
using eerie_leap::domain::logging_domain::loggers::Mdf4LoggerSensorReading;
using eerie_leap::domain::system_domain::models::SystemConfiguration;
using eerie_leap::subsys::event_bus::CreateScopedSubscription;
using eerie_leap::subsys::threading::ServiceState;

LOG_MODULE_REGISTER(log_writer_supervisor_logger);

struct LogWriterSupervisor::State {
    std::shared_ptr<LogWriterService> log_writer_service;
    // The thread every transition below is meant to run on.
    std::shared_ptr<WorkQueueThread> config_work_queue_thread;

    // Written by any thread, taken by the task.
    std::atomic<Request> pending_request = Request::None;
    // Set by a successful start, cleared by a stop or a failure: whether a sensors restart brings the
    // writer back. Configuration work queue only; atomic so a dependent called from elsewhere is no
    // data race.
    std::atomic<bool> is_logging_requested = false;
    std::atomic<bool> is_off_queue_reported = false;

    // Both log and return false when the transition failed or threw.
    bool StartWriter() {
        try {
            return log_writer_service->Start();
        } catch(const std::exception& e) {
            LOG_ERR("Failed to start the log writer: %s", e.what());
        } catch(...) {
            LOG_ERR("Failed to start the log writer.");
        }

        return false;
    }

    bool StopWriter() {
        try {
            if(log_writer_service->Stop())
                return true;

            LOG_ERR("Failed to stop the log writer.");
        } catch(const std::exception& e) {
            LOG_ERR("Failed to stop the log writer: %s", e.what());
        } catch(...) {
            LOG_ERR("Failed to stop the log writer.");
        }

        return false;
    }

    // SensorsController restarts processing on the queue it was given; a different one would let a
    // restart interleave with the requests.
    void CheckThread() {
        if(!config_work_queue_thread->IsCurrentThread() && !is_off_queue_reported.exchange(true))
            LOG_ERR("The log writer's dependent runs off the configuration work queue.");
    }
};

// The writer as SensorsController stops and starts it around a restart of sensor processing. The
// restart runs on the configuration work queue, in turn with the requests, so a request that came in
// meanwhile is still pending when Start() is called, and decides.
class LogWriterSupervisor::DependentLogWriter final : public IService {
private:
    std::shared_ptr<State> state_;

public:
    explicit DependentLogWriter(std::shared_ptr<State> state) :
        state_(std::move(state)) {}

    bool Initialize() override {
        return true;
    }

    bool Start() override {
        state_->CheckThread();

        // Stopped meanwhile, or about to be: the latest request wins.
        if(!state_->is_logging_requested || state_->pending_request == Request::Stop)
            return true;

        // A session that failed during the restart stays down; the next start is a request's.
        if(state_->log_writer_service->HasFailed()) {
            state_->is_logging_requested = false;
            return true;
        }

        state_->is_logging_requested = state_->StartWriter();

        return state_->is_logging_requested;
    }

    // Leaves the wanted state as it is, for Start().
    bool Stop() override {
        state_->CheckThread();

        return state_->StopWriter();
    }

    // A failed session is RUNNING until it is stopped, so the restart stops it too.
    [[nodiscard]] ServiceState GetState() const noexcept override {
        return state_->log_writer_service->GetState();
    }
};

namespace {

LoggerMetadata CreateLoggerMetadata(
    SensorsConfigurationManager& sensors_configuration_manager,
    const std::string& tool_id,
    const LogWriterSupervisor::DeviceIdProvider& device_id_provider
) {

    LoggerMetadata metadata;

    metadata.device_name = CONFIG_BOARD;
    metadata.tool_id = tool_id;
    // The same version as the sw_version property, so a file names one software version.
    metadata.tool_version = std::to_string(CONFIG_EERIE_LEAP_SW_VERSION_MAJOR) + "."
        + std::to_string(CONFIG_EERIE_LEAP_SW_VERSION_MINOR) + "." + std::to_string(CONFIG_EERIE_LEAP_SW_VERSION_PATCH);
    metadata.product_id = CONFIG_EERIE_LEAP_PRODUCT_ID;
    metadata.hw_version = SystemConfiguration::HW_VERSION;
    metadata.sw_version = SystemConfiguration::SW_VERSION;

    // Unassigned (0) until the unit has claimed an ID on the CDMP network, or without one.
    if(device_id_provider)
        metadata.device_id = device_id_provider();

    // The CRC of the stored CBOR, so a log can be matched with the configuration that produced it.
    // A stored configuration that was rejected at boot is not what runs: 0, unknown, instead.
    const auto configuration_info = sensors_configuration_manager.GetCborConfigurationInfo();
    metadata.sensors_configuration_hash = configuration_info.is_applied ? configuration_info.crc : 0;

    return metadata;
}

// Run by every start, so each file describes the configurations and the device ID in effect when it
// was opened. It holds what it needs, not the supervisor, so a writer started after the supervisor
// is gone (by the dependent) still gets its logger.
LogWriterService::LoggerFactory MakeLoggerFactory(
    std::shared_ptr<LoggingConfigurationManager> logging_configuration_manager,
    std::shared_ptr<SensorsConfigurationManager> sensors_configuration_manager,
    std::string tool_id,
    LogWriterSupervisor::DeviceIdProvider device_id_provider
) {

    return [logging_configuration_manager = std::move(logging_configuration_manager),
               sensors_configuration_manager = std::move(sensors_configuration_manager), tool_id = std::move(tool_id),
               device_id_provider = std::move(device_id_provider)] -> std::shared_ptr<ILogger<SensorReading>> {
        const auto sensors = sensors_configuration_manager->Get();
        const auto logging_configuration = logging_configuration_manager->Get();

        if(sensors == nullptr || logging_configuration == nullptr)
            return nullptr;

        return std::make_shared<Mdf4LoggerSensorReading>(
            *logging_configuration, *sensors,
            CreateLoggerMetadata(*sensors_configuration_manager, tool_id, device_id_provider)
        );
    };
}

} // namespace

LogWriterSupervisor::LogWriterSupervisor(
    std::shared_ptr<WorkQueueThread> config_work_queue_thread,
    std::shared_ptr<LogWriterService> log_writer_service,
    std::shared_ptr<LoggingConfigurationManager> logging_configuration_manager,
    std::shared_ptr<SensorsConfigurationManager> sensors_configuration_manager,
    std::string tool_id,
    DeviceIdProvider device_id_provider
) :
    config_work_queue_thread_(std::move(config_work_queue_thread)),
    logging_configuration_manager_(std::move(logging_configuration_manager)),
    sensors_configuration_manager_(std::move(sensors_configuration_manager)),
    tool_id_(std::move(tool_id)),
    device_id_provider_(std::move(device_id_provider)),
    state_(std::make_shared<State>()),
    guard_(std::make_shared<DispatchGuard>(this)) {

    state_->log_writer_service = std::move(log_writer_service);
    state_->config_work_queue_thread = config_work_queue_thread_;
    dependent_ = std::make_shared<DependentLogWriter>(state_);
}

LogWriterSupervisor::~LogWriterSupervisor() {
    // A handler the channel or the manager already called on another thread becomes a no-op, and
    // one in its body is waited for; neither reschedules the task afterwards.
    guard_->Detach();

    status_subscription_.reset();

    if(is_handler_registered_)
        logging_configuration_manager_->RegisterConfigurationUpdatedHandler(nullptr);

    // Drops a pending run and waits for one in progress, which may be inside a start or a stop.
    if(task_.has_value())
        task_->Cancel();
}

bool LogWriterSupervisor::Initialize() {
    if(is_initialized_)
        return true;

    try {
        task_ = config_work_queue_thread_->CreateTask(
            [](LogWriterSupervisor* supervisor) {
                return supervisor->Process();
            },
            this
        );
    } catch(const std::exception& e) {
        LOG_ERR("Failed to create the log writer supervisor's task: %s", e.what());
        return false;
    }

    state_->log_writer_service->SetLoggerFactory(
        MakeLoggerFactory(logging_configuration_manager_, sensors_configuration_manager_, tool_id_, device_id_provider_)
    );

    // The composition root decides which bus carries the channel. Until one does, IsActive() stays
    // false and a failed session is stopped only by the task's next run (a request or a stored
    // logging configuration) or by a sensors restart.
    auto& channel = LoggingEventsChannel::GetInstance();
    if(!channel.IsRegistered())
        LOG_WRN("No bus carries logging_events yet; status and failures go unseen until one does.");

    // The writer publishes asynchronously, so this runs on the bus thread, never on the writer's.
    status_subscription_ = CreateScopedSubscription(
        channel, LoggingEventType::StatusUpdated,
        [guard = guard_, this](const LoggingEventsChannel::EventMessage& event) {
            guard->Dispatch([&] {
                OnStatusUpdated(event);
            });
        }
    );
    if(status_subscription_ == nullptr) {
        LOG_ERR("Failed to subscribe to the log writer status.");
        return false;
    }

    // Runs on the thread that stored the configuration (an SMP apply runs on the configuration work
    // queue), so the restart is queued behind it.
    logging_configuration_manager_->RegisterConfigurationUpdatedHandler([guard = guard_, this] {
        guard->Dispatch([this] {
            is_restart_due_ = true;
            task_->Reschedule(K_NO_WAIT);
        });
    });
    is_handler_registered_ = true;

    is_initialized_ = true;

    return true;
}

void LogWriterSupervisor::RequestStart(uint32_t /*target_uid*/) {
    if(!is_initialized_) {
        LOG_ERR("Logging start requested before the supervisor was initialized.");
        return;
    }

    state_->pending_request = Request::Start;
    if(task_.has_value())
        task_->Reschedule(K_NO_WAIT);
}

void LogWriterSupervisor::RequestStop(uint32_t /*target_uid*/) {
    if(!is_initialized_) {
        LOG_ERR("Logging stop requested before the supervisor was initialized.");
        return;
    }

    state_->pending_request = Request::Stop;
    if(task_.has_value())
        task_->Reschedule(K_NO_WAIT);
}

bool LogWriterSupervisor::HandleLoggingCommand(bool is_start) {
    if(is_start)
        RequestStart(ALL_UNITS);
    else
        RequestStop(ALL_UNITS);

    return true;
}

bool LogWriterSupervisor::IsActive() const {
    return is_active_;
}

LoggingStatusReason LogWriterSupervisor::GetReason() const {
    return reason_;
}

std::shared_ptr<IService> LogWriterSupervisor::GetDependentService() const {
    return dependent_;
}

// One task does it all, in a fixed order: a failed session is closed before a request acts, and a
// restart for a stored configuration comes last, unless this run opened a session with it.
WorkQueueTaskResult LogWriterSupervisor::Process() {
    // Taken first. The handler sets it after the new configuration is in place, so a start below
    // reads that configuration; one set while this run starts the writer stays for the next run,
    // which the handler has queued.
    const bool is_restart_due = is_restart_due_.exchange(false);

    StopIfFailed();

    bool is_new_session = false;
    switch(state_->pending_request.exchange(Request::None)) {
        case Request::Start:
            is_new_session = ApplyStart();
            break;
        case Request::Stop:
            ApplyStop();
            break;
        case Request::None:
            break;
    }

    if(is_restart_due && !is_new_session)
        RestartIfRunning();

    return {};
}

// A session that failed (WriteFailed, NoSpace or RotationFailed) is stopped here, off the writer
// thread: the stop finalizes what it can and closes the file. The service's own failure flag decides
// rather than the event's reason, so a failure event that arrives after a new start stops nothing.
void LogWriterSupervisor::StopIfFailed() {
    // Taken by every check, so a late event of a session that is gone names no later failure.
    const auto reason = failure_reason_.exchange(LoggingStatusReason::Stopped);
    const auto& writer = *state_->log_writer_service;

    if(!writer.HasFailed() || !writer.IsRunning())
        return;

    // A run for a request can find the session failed before its event arrived; the writer has
    // logged why either way.
    if(reason != LoggingStatusReason::Stopped)
        LOG_ERR("Logging failed (%s); stopping the log writer.", GetReasonName(reason));
    else
        LOG_ERR("Logging failed; stopping the log writer.");

    state_->is_logging_requested = false;
    state_->StopWriter();
}

bool LogWriterSupervisor::ApplyStart() {
    const auto& writer = *state_->log_writer_service;

    if(writer.IsRunning()) {
        if(!writer.HasFailed()) {
            state_->is_logging_requested = true;
            return false;
        }

        // A session that failed since StopIfFailed() is closed before the next one opens. A start
        // that cannot close it starts nothing, as the writer would only report RUNNING.
        if(!state_->StopWriter()) {
            state_->is_logging_requested = false;
            LOG_ERR("Logging not started: the failed session could not be stopped.");
            return false;
        }
    }

    const bool is_started = state_->StartWriter();
    state_->is_logging_requested = is_started;

    if(!is_started)
        LOG_ERR("Failed to start logging.");

    return is_started;
}

void LogWriterSupervisor::ApplyStop() {
    state_->is_logging_requested = false;
    state_->StopWriter();
}

// A stored logging configuration takes effect at the next start, so a running writer is restarted:
// the file it was writing is finalized and the next one is laid out for the new configuration.
void LogWriterSupervisor::RestartIfRunning() {
    const auto& writer = *state_->log_writer_service;

    if(!writer.IsRunning() || writer.HasFailed())
        return;

    if(!state_->StopWriter()) {
        LOG_ERR("Failed to stop logging for the new configuration.");
        return;
    }

    state_->is_logging_requested = state_->StartWriter();
    if(!state_->is_logging_requested)
        LOG_ERR("Failed to restart logging with the new configuration.");
}

void LogWriterSupervisor::OnStatusUpdated(const LoggingEventsChannel::EventMessage& event) {
    const auto* const active = event.payload.find(LoggingPayloadType::IsActive);
    if(active == event.payload.end() || !std::holds_alternative<bool>(active->second))
        return;

    const bool is_active = std::get<bool>(active->second);
    is_active_ = is_active;
    if(is_active) {
        reason_ = LoggingStatusReason::Stopped;
        return;
    }

    auto reason = LoggingStatusReason::Stopped;
    const auto* const reason_entry = event.payload.find(LoggingPayloadType::Reason);
    if(reason_entry != event.payload.end() && std::holds_alternative<uint32_t>(reason_entry->second))
        reason = static_cast<LoggingStatusReason>(std::get<uint32_t>(reason_entry->second));
    reason_ = reason;

    // A stop publishes Stopped; anything else ended a session that the task now closes.
    if(reason == LoggingStatusReason::Stopped)
        return;

    failure_reason_ = reason;
    if(task_.has_value())
        task_->Reschedule(K_NO_WAIT);
}

} // namespace eerie_leap::domain::logging_domain::services
