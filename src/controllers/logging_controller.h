#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <zephyr/kernel.h>

#include "subsys/event_bus/dispatch_guard.h"
#include "subsys/fs/services/i_fs_service.h"
#include "subsys/threading/work_queue_task.h"
#include "subsys/threading/work_queue_thread.h"
#include "subsys/time/i_time_service.h"

#include "domain/configuration_domain/services/configuration_service.h"
#include "domain/canbus_com_domain/services/canbus_com_service.h"
#include "domain/sensor_domain/configuration/sensors_configuration_manager.h"
#include "domain/sensor_domain/services/sensor_events_publisher.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "domain/logging_domain/event_bus/logging_network_state.h"
#include "domain/logging_domain/control/canbus_com_logging_availability_reporter.h"
#include "domain/logging_domain/control/configuration_update_logging_stop.h"
#include "domain/logging_domain/control/i_logging_control.h"
#include "domain/logging_domain/control/logging_command_handler.h"
#include "domain/logging_domain/control/logging_trigger_set.h"
#include "domain/logging_domain/control/remote_logging_control.h"
#include "domain/logging_domain/control/configuration/logging_control_configuration_manager.h"

// The role headers are conditional: the log writer's header reads logging domain options.
#ifdef CONFIG_EERIE_LEAP_DOMAIN_LOGGING
#include "subsys/threading/i_service.h"
#include "domain/canbus_com_domain/state_bridge/network_state_exporter.h"
#include "domain/logging_domain/configuration/logging_configuration_manager.h"
#include "domain/logging_domain/services/log_writer_service.h"
#include "domain/logging_domain/services/log_writer_supervisor.h"
#include "domain/logging_domain/control/writer_logging_control.h"
#else
#include "domain/canbus_com_domain/state_bridge/network_state_importer.h"
#endif

namespace eerie_leap::subsys::assets {
class AssetsManager;
}

namespace eerie_leap::controllers {

using eerie_leap::subsys::assets::AssetsManager;
using eerie_leap::subsys::event_bus::DispatchGuard;
using eerie_leap::subsys::fs::services::IFsService;
using eerie_leap::subsys::threading::WorkQueueTask;
using eerie_leap::subsys::threading::WorkQueueTaskResult;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::subsys::time::ITimeService;

using eerie_leap::domain::configuration_domain::services::ConfigurationService;
using eerie_leap::domain::canbus_com_domain::services::CanbusComService;
using eerie_leap::domain::sensor_domain::configuration::SensorsConfigurationManager;
using eerie_leap::domain::sensor_domain::services::SensorEventsPublisher;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;
using eerie_leap::domain::logging_domain::control::CanbusComLoggingAvailabilityReporter;
using eerie_leap::domain::logging_domain::control::ConfigurationUpdateLoggingStop;
using eerie_leap::domain::logging_domain::control::ILoggingControl;
using eerie_leap::domain::logging_domain::control::LoggingCommandHandler;
using eerie_leap::domain::logging_domain::control::LoggingTriggerSet;
using eerie_leap::domain::logging_domain::control::RemoteLoggingControl;
using eerie_leap::domain::logging_domain::control::configuration::LoggingControlConfigurationManager;
using eerie_leap::domain::logging_domain::event_bus::LoggingNetworkState;

#ifdef CONFIG_EERIE_LEAP_DOMAIN_LOGGING
using eerie_leap::subsys::threading::IService;

using eerie_leap::domain::canbus_com_domain::state_bridge::NetworkStateExporter;
using eerie_leap::domain::logging_domain::configuration::LoggingConfigurationManager;
using eerie_leap::domain::logging_domain::services::LogWriterService;
using eerie_leap::domain::logging_domain::control::WriterLoggingControl;
using eerie_leap::domain::logging_domain::services::LogWriterSupervisor;
#else
using eerie_leap::domain::canbus_com_domain::state_bridge::NetworkStateImporter;
#endif

// Starts and stops logging, by triggers, by the CDMP LOGGING command and through GetControl(). The
// role is the build's: with EERIE_LEAP_DOMAIN_LOGGING the unit writes logs (writer role), without it
// the unit controls the units that do (remote role).
//
// Build it once logging_events and sensor_events each have a bus (one may carry both) and the channel
// registry names them, after CanbusController and SensorsController::Initialize() (it takes the
// sensors manager), before ManagementController (it registers configuration managers) and before
// SensorsController::Start() (its triggers subscribe first). Destroy it from the thread that built
// it, not from the configuration work queue, a bus thread or the CDMP work queue: it waits for all
// of them. Build it once per ConfigurationService: the service cannot unregister a manager, so
// Initialize() fails on one that already holds the logging types.
//
// A configuration stored on this unit after Start() stops logging on every unit (L9): the unit's own
// control stops, the triggers take a baseline, and the LOGGING stop is broadcast. The composition
// root calls Start() once the unit has finished starting.
class LoggingController {
public:
    // TODO: For test purposes only
    using ConfigurationSetup = std::function<void(std::shared_ptr<LoggingControlConfigurationManager>)>;

    /**
     * @param config_work_queue_thread Running; the queue SensorsController was given, so a sensors
     *        restart and the writer's transitions run in turn.
     * @param configuration_service Null: the managers are not registered. One that already holds
     *        a logging type fails Initialize().
     * @param sensors_configuration_manager The writer's logger metadata and sensors (writer role,
     *        required) and the check that a trigger's sensor exists (null: not checked).
     * @param canbus_com_service Remote role: required. Writer role: null logs on its own, without the
     *        CDMP command and without sharing the status.
     * @param sensor_events_publisher Replays the frame after the triggers are rebuilt; null: a rebuilt
     *        trigger waits for the next change of its value.
     * @param sd_fs_service The log volume; writer role only, required there.
     * @param time_service, sensor_readings_frame The log writer's; writer role only, required there.
     * @param tool_id The log header's tool ID, such as "rt_daq"; writer role only.
     * @param assets_manager Writer role with CONFIG_EERIE_LEAP_ASSETS: the registry the log files are
     *        registered with as the LOGS asset kind once Initialize() succeeded; the unregistration
     *        is Teardown()'s. Null: not registered. The remote role writes no logs and registers
     *        nothing.
     */
    LoggingController(
        std::shared_ptr<IFsService> fs_service,
        std::shared_ptr<WorkQueueThread> config_work_queue_thread,
        std::shared_ptr<ConfigurationService> configuration_service,
        std::shared_ptr<SensorsConfigurationManager> sensors_configuration_manager,
        std::shared_ptr<CanbusComService> canbus_com_service,
        std::shared_ptr<SensorEventsPublisher> sensor_events_publisher,
        std::shared_ptr<IFsService> sd_fs_service = nullptr,
        std::shared_ptr<ITimeService> time_service = nullptr,
        std::shared_ptr<SensorReadingsFrame> sensor_readings_frame = nullptr,
        std::string tool_id = {},
        std::shared_ptr<AssetsManager> assets_manager = nullptr);

    // Withdraws everything that refers to it, in an order that is safe while the bus threads and
    // the configuration and CDMP work queues run. The writer role stops logging first and waits for
    // the stop. Safe after a failed or skipped Initialize().
    ~LoggingController();

    // The rebuild task and the updated handler refer to it.
    LoggingController(const LoggingController&) = delete;
    LoggingController& operator=(const LoggingController&) = delete;
    LoggingController(LoggingController&&) = delete;
    LoggingController& operator=(LoggingController&&) = delete;

    // Once. 0 on success. -1 leaves every getter null and registers nothing; a second call repeats
    // the first one's result.
    int Initialize(const ConfigurationSetup& setup_test_configuration = nullptr);

    /**
     * From now on a configuration stored on this unit stops logging on every unit (L9). Call it once
     * the unit has finished starting, after every store of its boot (defaults, the test hooks), and
     * before ManagementController lets other units write configurations; stores before it stop
     * nothing. 0 on success. -1 after a failed Initialize() (which logged why), and with an error
     * before Initialize(). Once; a second call logs an error and returns the first result. Without a
     * configuration service it returns 0 and no store stops logging.
     */
    int Start();

    // The role's ILoggingControl: WriterLoggingControl (the writer, and the other units through the
    // network) or RemoteLoggingControl. Whoever keeps it releases it before the controller goes, so no
    // request comes in after the teardown.
    [[nodiscard]] std::shared_ptr<ILoggingControl> GetControl() const { return logging_control_; }
    // This unit's CDMP UID, 0 without a COM service (or before the network knows it): what a request
    // passes to act on this unit alone, such as a shell command or an autostart on a writer.
    [[nodiscard]] uint32_t GetUnitUid() const;
    [[nodiscard]] std::shared_ptr<LoggingControlConfigurationManager> GetLoggingControlConfigurationManager() const {
        return logging_control_configuration_manager_;
    }

#ifdef CONFIG_EERIE_LEAP_DOMAIN_LOGGING
    // For SensorsController::RegisterDependentService(), only after Initialize() returned 0.
    [[nodiscard]] std::shared_ptr<IService> GetDependentService() const;
    [[nodiscard]] std::shared_ptr<LoggingConfigurationManager> GetConfigurationManager() const {
        return logging_configuration_manager_;
    }
    [[nodiscard]] std::shared_ptr<LogWriterService> GetLogWriterService() const { return log_writer_service_; }
#endif

private:
    static constexpr const char* LOGGING_CONTROL_CONFIGURATION_NAME = "logging_control_config";
#ifdef CONFIG_EERIE_LEAP_DOMAIN_LOGGING
    // The file name rt_daq already uses, so its stored logging configuration still loads.
    static constexpr const char* LOGGING_CONFIGURATION_NAME = "logging_config";
    using LoggingStatusExporter = NetworkStateExporter<LoggingNetworkState>;
#else
    using LoggingStatusImporter = NetworkStateImporter<LoggingNetworkState>;
#endif

    // Given.
    std::shared_ptr<IFsService> fs_service_;
    std::shared_ptr<WorkQueueThread> config_work_queue_thread_;   // Declared before rebuild_task_.
    std::shared_ptr<ConfigurationService> configuration_service_;
    std::shared_ptr<SensorsConfigurationManager> sensors_configuration_manager_;
    std::shared_ptr<CanbusComService> canbus_com_service_;
    std::shared_ptr<SensorEventsPublisher> sensor_events_publisher_;
    std::shared_ptr<IFsService> sd_fs_service_;
    std::shared_ptr<ITimeService> time_service_;
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame_;
    std::string tool_id_;
    std::shared_ptr<AssetsManager> assets_manager_;

    // Built by Initialize(), dropped by Teardown().
    std::shared_ptr<LoggingControlConfigurationManager> logging_control_configuration_manager_;
#ifdef CONFIG_EERIE_LEAP_DOMAIN_LOGGING
    std::shared_ptr<LoggingConfigurationManager> logging_configuration_manager_;
    std::shared_ptr<LogWriterService> log_writer_service_;
    std::shared_ptr<LogWriterSupervisor> log_writer_supervisor_;
    std::unique_ptr<LoggingStatusExporter> logging_status_exporter_;
    // The network half of the writer's control (Mode::Peer); null without a COM service.
    std::shared_ptr<RemoteLoggingControl> network_logging_control_;
    std::unique_ptr<CanbusComLoggingAvailabilityReporter> logging_availability_reporter_;
#else
    std::shared_ptr<RemoteLoggingControl> remote_logging_control_;
    std::unique_ptr<LoggingStatusImporter> logging_status_importer_;
#endif
    // The role's control, set once the role's own Initialize() succeeded.
    std::shared_ptr<ILoggingControl> logging_control_;
    // Serves the LOGGING command in both roles; the control decides.
    std::unique_ptr<LoggingCommandHandler> logging_command_handler_;
    std::unique_ptr<ConfigurationUpdateLoggingStop> configuration_update_logging_stop_;

    // Replaced only by RebuildTriggers() on the configuration work queue, and by Initialize() and
    // Teardown() while neither the task nor the handler can run; trigger_set_lock_ orders those
    // against an update's report.
    std::unique_ptr<LoggingTriggerSet> logging_trigger_set_;
    // logging_trigger_set_ between RebuildTriggers() (configuration work queue) and an update's report
    // (the storing thread). Taken after the update stop's guard, and the set's guard after it.
    k_mutex trigger_set_lock_;
    // Set by the first update after Start(): from then on a rebuilt set takes first values as
    // baselines. Read by RebuildTriggers() under trigger_set_lock_.
    std::atomic<bool> is_configuration_updated_ = false;
    std::optional<WorkQueueTask<LoggingController>> rebuild_task_;
    // The updated handler's way back to this; created with the controller so Teardown() can detach
    // it from any state, and detached first.
    std::shared_ptr<DispatchGuard> guard_;
    bool is_handler_registered_ = false;
    // Whether Build() registered the LOGS kind with assets_manager_, which Teardown() unregisters.
    bool is_assets_registered_ = false;
    bool is_initialize_called_ = false;
    bool is_initialized_ = false;
    bool is_start_called_ = false;
    int start_result_ = -1;

    // Logs what is missing.
    bool HasDependencies() const;
    // Throws on a construction failure; Initialize() tears down after it.
    bool Build(const ConfigurationSetup& setup_test_configuration);
    // The writer's or the remote's pieces, and logging_control_.
    bool BuildRole();
    // Always a set: without a stored configuration it has no triggers.
    std::unique_ptr<LoggingTriggerSet> BuildTriggerSet(LoggingTriggerSet::FirstValue first_value) const;
    void ReplayFor(const LoggingTriggerSet& set) const;
    // The rebuild task; configuration work queue only.
    void RebuildTriggers();
    // The update stop's UpdateHandler, inside its guard, on the storing thread: baselines for every
    // trigger, and for a sensors update the withdrawal of triggers whose sensor is gone.
    void OnConfigurationUpdated(ConfigurationService::Type type);
    // Writer role with CONFIG_EERIE_LEAP_ASSETS, a manager and the log volume: serves the log files as
    // the LOGS kind, reading and deleting them, but not the one being written. Does nothing otherwise.
    void RegisterAssets();
    void UnregisterAssets();
#ifdef CONFIG_EERIE_LEAP_DOMAIN_LOGGING
    // From Teardown(): stops logging through the supervisor and waits for it.
    void StopLogWriter();
#endif
    void Teardown();
};

} // namespace eerie_leap::controllers
