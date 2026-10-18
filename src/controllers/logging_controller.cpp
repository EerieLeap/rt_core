#include <algorithm>
#include <cstdint>
#include <exception>
#include <span>
#include <utility>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "subsys/event_bus/event.h"
#include "subsys/event_bus/event_binding.h"
#include "subsys/event_bus/event_binding_subscription.h"
#include "subsys/event_bus/event_channel_id.h"
#include "subsys/threading/scoped_mutex.h"

#include "configuration/cbor/cbor_logging_control_config/cbor_logging_control_config.h"
#include "configuration/services/cbor_configuration_service.h"

#include "domain/logging_domain/control/models/logging_control_configuration.h"
#include "domain/logging_domain/control/models/logging_trigger.h"
#include "domain/sensor_domain/event_bus/sensor_payload_type.h"

#include "domain/logging_domain/control/canbus_com_logging_command_sender.h"
#include "domain/logging_domain/control/canbus_com_logging_restart_monitor.h"

#ifdef CONFIG_EERIE_LEAP_DOMAIN_LOGGING
    #include "configuration/cbor/cbor_logging_config/cbor_logging_config.h"
#endif

#if defined(CONFIG_EERIE_LEAP_ASSETS) && defined(CONFIG_EERIE_LEAP_DOMAIN_LOGGING)
    #include <array>
    #include <string_view>

    #include "subsys/assets/asset_kind.h"
    #include "subsys/assets/asset_provider.h"
    #include "subsys/assets/asset_store.h"
    #include "subsys/assets/assets_manager.h"
#endif

#include "logging_controller.h"

namespace eerie_leap::controllers {

using eerie_leap::domain::logging_domain::control::CanbusComLoggingCommandSender;
using eerie_leap::domain::logging_domain::control::CanbusComLoggingRestartMonitor;
using eerie_leap::domain::logging_domain::control::ILoggingCommandSender;
using eerie_leap::domain::logging_domain::control::models::LoggingTrigger;
using eerie_leap::domain::sensor_domain::event_bus::SensorPayloadType;
using eerie_leap::subsys::event_bus::EventBinding;
using eerie_leap::subsys::event_bus::EventChannelId;
using eerie_leap::subsys::event_bus::EventData;
using eerie_leap::subsys::event_bus::SelectorMatches;
using eerie_leap::subsys::event_bus::ToSelectorValue;
using eerie_leap::subsys::threading::ScopedMutex;
#if defined(CONFIG_EERIE_LEAP_ASSETS) && defined(CONFIG_EERIE_LEAP_DOMAIN_LOGGING)
using eerie_leap::domain::logging_domain::services::LogFileNaming;
#endif

namespace config_services = eerie_leap::configuration::services;

LOG_MODULE_REGISTER(logging_controller_logger);

namespace {

// Whether a binding to a sensor names an enabled sensor of the stored configuration; any other
// binding is not checked. The selector is judged by the subscription's own rule, so text and integer
// selectors count as they would match. It runs on the building thread (boot: the composition root's;
// a rebuild: the configuration work queue), and the sensors are loaded by then. At boot a sensor that
// appears later is not re-checked: the trigger is subscribed by hash and works as soon as it is
// published, so the warning is informational (L6). The resolver runs again at each sensors update
// (OnConfigurationUpdated()), which withdraws a trigger whose sensor is gone. Null manager: null
// resolver.
LoggingTriggerSet::SelectorResolver MakeSelectorResolver(std::shared_ptr<SensorsConfigurationManager> sensors_manager) {
    if(sensors_manager == nullptr)
        return nullptr;

    return [manager = std::move(sensors_manager)](const EventBinding& binding) {
        if(binding.channel != EventChannelId::Sensors
            || !binding.HasSelector()
            || binding.selector_key != std::to_underlying(SensorPayloadType::SensorId))
            return true;

        const auto selector = ToSelectorValue(binding.selector_value);
        if(!selector.has_value())
            return true; // The set logs a selector that cannot select.

        const auto sensors = manager->Get();
        if(sensors == nullptr)
            return false;

        return std::ranges::any_of(*sensors, [&selector](const auto& sensor) {
            return sensor->is_enabled && SelectorMatches(*selector, EventData(sensor->id_hash));
        });
    };
}

#if defined(CONFIG_EERIE_LEAP_ASSETS) && defined(CONFIG_EERIE_LEAP_DOMAIN_LOGGING)
// The extension of a log file, in static storage as AssetProvider::extensions needs.
constexpr std::array<std::string_view, 1> LOG_EXTENSIONS = { ".mf4" };

// What the writer adds to the prefix: "_YYYYMMDDTHHMMSSZ_NNNN.mf4".
constexpr size_t LOG_NAME_SUFFIX_LENGTH = sizeof("_YYYYMMDDTHHMMSSZ_NNNN.mf4") - 1;

// Whether the writer's file names pass AssetStore::IsValidName() for the LOGS kind: the longest one fits
// the name length of the assets, and it holds only the characters a name may, not starting with a dot.
constexpr bool IsLogPrefixValid(std::string_view prefix) {
    if(prefix.size() + LOG_NAME_SUFFIX_LENGTH > CONFIG_EERIE_LEAP_ASSETS_MAX_NAME_LENGTH)
        return false;

    if(!prefix.empty() && prefix.front() == '.')
        return false;

    for(const char c : prefix) {
        const bool is_name_character = (c >= 'a' && c <= 'z')
            || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9')
            || c == '.'
            || c == '_'
            || c == '-';
        if(!is_name_character)
            return false;
    }

    return true;
}

static_assert(LOG_NAME_SUFFIX_LENGTH == 26, "The writer adds 26 characters to the prefix.");
static_assert(
    IsLogPrefixValid(CONFIG_EERIE_LEAP_LOG_DATA_FILE_PREFIX),
    "EERIE_LEAP_LOG_DATA_FILE_PREFIX plus the 26 characters the writer adds must be a valid asset name: "
    "at most EERIE_LEAP_ASSETS_MAX_NAME_LENGTH bytes of [A-Za-z0-9._-], not starting with a dot."
);
#endif

#ifdef CONFIG_EERIE_LEAP_DOMAIN_LOGGING
// The log header's device ID. The writer's logger factory keeps the provider as long as the writer
// lives, which a dependent of SensorsController can stretch past the controller, so it holds the COM
// service weakly. 0 without a service, or before it claimed an ID.
LogWriterSupervisor::DeviceIdProvider
MakeDeviceIdProvider(const std::shared_ptr<CanbusComService>& canbus_com_service) {
    if(canbus_com_service == nullptr)
        return nullptr;

    return [weak_com = std::weak_ptr<CanbusComService>(canbus_com_service)]() -> uint8_t {
        const auto com = weak_com.lock();
        const auto network_info = com != nullptr ? com->GetNetworkInfo() : nullptr;

        return network_info != nullptr ? network_info->GetDeviceInfo().device_id : uint8_t{ 0 };
    };
}
#endif

} // namespace

LoggingController::LoggingController(
    std::shared_ptr<IFsService> fs_service,
    std::shared_ptr<WorkQueueThread> config_work_queue_thread,
    std::shared_ptr<ConfigurationService> configuration_service,
    std::shared_ptr<SensorsConfigurationManager> sensors_configuration_manager,
    std::shared_ptr<CanbusComService> canbus_com_service,
    std::shared_ptr<SensorEventsPublisher> sensor_events_publisher,
    std::shared_ptr<IFsService> sd_fs_service,
    std::shared_ptr<ITimeService> time_service,
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
    std::string tool_id,
    std::shared_ptr<AssetsManager> assets_manager
) :
    fs_service_(std::move(fs_service)),
    config_work_queue_thread_(std::move(config_work_queue_thread)),
    configuration_service_(std::move(configuration_service)),
    sensors_configuration_manager_(std::move(sensors_configuration_manager)),
    canbus_com_service_(std::move(canbus_com_service)),
    sensor_events_publisher_(std::move(sensor_events_publisher)),
    sd_fs_service_(std::move(sd_fs_service)),
    time_service_(std::move(time_service)),
    sensor_readings_frame_(std::move(sensor_readings_frame)),
    tool_id_(std::move(tool_id)),
    assets_manager_(std::move(assets_manager)),
    guard_(std::make_shared<DispatchGuard>(this)) {

    k_mutex_init(&trigger_set_lock_);
}

LoggingController::~LoggingController() {
    Teardown();
}

int LoggingController::Initialize(const ConfigurationSetup& setup_test_configuration) {
    if(is_initialize_called_) {
        LOG_ERR("The logging controller is initialized once.");
        return is_initialized_ ? 0 : -1;
    }
    is_initialize_called_ = true;

    if(!HasDependencies())
        return -1;

    try {
        if(Build(setup_test_configuration)) {
#ifdef CONFIG_EERIE_LEAP_DOMAIN_LOGGING
            LOG_INF("Logging controller initialized (writer role).");
#else
            LOG_INF("Logging controller initialized (remote role).");
#endif
            is_initialized_ = true;

            return 0;
        }
    } catch(const std::exception& e) {
        LOG_ERR("Failed to initialize the logging controller: %s", e.what());
    } catch(...) {
        LOG_ERR("Failed to initialize the logging controller.");
    }

    // Every getter is null, nothing is registered, and nothing refers to the controller.
    Teardown();

    return -1;
}

int LoggingController::Start() {
    if(is_start_called_) {
        LOG_ERR("The logging controller is started once.");
        return start_result_;
    }

    if(!is_initialize_called_) {
        LOG_ERR("The logging controller is started after Initialize().");
        return -1;
    }
    is_start_called_ = true;

    // Initialize() logged why.
    if(!is_initialized_)
        return start_result_ = -1;

    if(configuration_update_logging_stop_ == nullptr) {
        LOG_INF("No configuration service: a stored configuration stops no logging.");
        return start_result_ = 0;
    }

    start_result_ = configuration_update_logging_stop_->Start() ? 0 : -1;

    return start_result_;
}

uint32_t LoggingController::GetUnitUid() const {
    if(canbus_com_service_ == nullptr)
        return 0;

    const auto network_info = canbus_com_service_->GetNetworkInfo();

    return network_info != nullptr ? network_info->GetDeviceInfo().uid : 0;
}

bool LoggingController::HasDependencies() const {
    bool has_all = true;

    auto require = [&has_all](bool is_present, const char* what) {
        if(!is_present) {
            LOG_ERR("The logging controller needs %s.", what);
            has_all = false;
        }
    };

    require(fs_service_ != nullptr, "the internal file system service");
    require(config_work_queue_thread_ != nullptr, "the configuration work queue");

    // ConfigurationService keeps the first manager registered for a type and cannot unregister, so
    // a controller built on a service that holds one would report success while every apply went to
    // a manager nothing reacts to.
    if(configuration_service_ != nullptr) {
        bool is_registered = configuration_service_->IsRegistered(ConfigurationService::Type::LoggingControl);
#ifdef CONFIG_EERIE_LEAP_DOMAIN_LOGGING
        is_registered = is_registered || configuration_service_->IsRegistered(ConfigurationService::Type::Logging);
#endif
        if(is_registered) {
            LOG_ERR(
                "The configuration service already holds a logging configuration: build the logging controller once "
                "per configuration service."
            );
            has_all = false;
        }
    }

#ifdef CONFIG_EERIE_LEAP_DOMAIN_LOGGING
    require(sd_fs_service_ != nullptr, "the log volume");
    require(time_service_ != nullptr, "the time service");
    require(sensor_readings_frame_ != nullptr, "the sensor readings frame");
    require(sensors_configuration_manager_ != nullptr, "the sensors configuration manager");
#else
    require(canbus_com_service_ != nullptr, "the CAN COM service (the remote logging role)");
#endif

    return has_all;
}

bool LoggingController::Build(const ConfigurationSetup& setup_test_configuration) {
    // 1. The triggers' configuration, in both roles.
    logging_control_configuration_manager_ = std::make_shared<LoggingControlConfigurationManager>(
        std::make_unique<config_services::CborConfigurationService<CborLoggingControlConfig>>(
            LOGGING_CONTROL_CONFIGURATION_NAME, fs_service_, config_work_queue_thread_
        )
    );

    // 2. Before the set is built and before the updated handler exists, so its store neither
    //    rebuilds nor replays: the set built below already reads it.
    // TODO: For test purposes only
    if(setup_test_configuration)
        setup_test_configuration(logging_control_configuration_manager_);

    // 3. The role, and with it the control the triggers request from.
    if(!BuildRole())
        return false;

    // 4. A stored configuration rebuilds the triggers on the configuration work queue, not on the
    //    thread that stored it: stores coalesce, return at once, and teardown can cancel the rebuild.
    //    The role's own Initialize() has already refused a queue that does not run.
    rebuild_task_ = config_work_queue_thread_->CreateTask(
        [](LoggingController* controller) {
            controller->RebuildTriggers();
            return WorkQueueTaskResult{};
        },
        this
    );

    // 5. After the role's pieces, so the first request reaches an initialized control. The replay
    //    finds an empty frame in the documented order (sensors not started yet); it makes a
    //    controller built after SensorsController::Start() still see a switch that is already on.
    {
        ScopedMutex lock(trigger_set_lock_);

        logging_trigger_set_ = BuildTriggerSet(LoggingTriggerSet::FirstValue::OneStarts);
        ReplayFor(*logging_trigger_set_);
    }

    // 6. The handler may run on any thread that stores the configuration and only reschedules the
    //    task. It holds the guard, not this: a call the manager already took finishes harmlessly
    //    after Teardown() detached it.
    logging_control_configuration_manager_->RegisterConfigurationUpdatedHandler([guard = guard_, this] {
        guard->Dispatch([this] {
            rebuild_task_->Reschedule(K_NO_WAIT);
        });
    });
    is_handler_registered_ = true;

    // 7. A configuration stored on this unit stops logging on every unit once Start() is called (L9).
    //    It registers nothing before that, so it may come before the configuration types. In both roles
    //    the control's RequestStop(kAllUnits) sends the broadcast (the writer's network half, the remote
    //    control itself), so there is no sender of its own.
    if(configuration_service_ != nullptr) {
        configuration_update_logging_stop_ = std::make_unique<ConfigurationUpdateLoggingStop>(
            configuration_service_, config_work_queue_thread_, logging_control_, nullptr,
            [this](ConfigurationService::Type type) {
                OnConfigurationUpdated(type);
            }
        );
        if(!configuration_update_logging_stop_->Initialize()) {
            LOG_ERR("Failed to initialize the configuration update stop.");
            return false;
        }
    }

    // 8. Last, so a failed Initialize() registers nothing: ConfigurationService cannot unregister.
    if(configuration_service_ != nullptr) {
#ifdef CONFIG_EERIE_LEAP_DOMAIN_LOGGING
        configuration_service_->RegisterCborConfigurationManager(
            ConfigurationService::Type::Logging, logging_configuration_manager_
        );
#endif
        configuration_service_->RegisterCborConfigurationManager(
            ConfigurationService::Type::LoggingControl, logging_control_configuration_manager_
        );
    }

    // 9. Nothing after this can fail, so a controller that failed to initialize serves nothing.
    RegisterAssets();

    return true;
}

#ifdef CONFIG_EERIE_LEAP_DOMAIN_LOGGING

bool LoggingController::BuildRole() {
    logging_configuration_manager_ = std::make_shared<LoggingConfigurationManager>(
        std::make_unique<config_services::CborConfigurationService<CborLoggingConfig>>(
            LOGGING_CONFIGURATION_NAME, fs_service_, config_work_queue_thread_
        )
    );

    // Initialized before the supervisor, which expects a stopped, initialized writer.
    log_writer_service_ = std::make_shared<LogWriterService>(
        sd_fs_service_, logging_configuration_manager_, time_service_, sensor_readings_frame_
    );
    if(!log_writer_service_->Initialize()) {
        LOG_ERR("Failed to initialize the log writer service.");
        return false;
    }

    log_writer_supervisor_ = std::make_shared<LogWriterSupervisor>(
        config_work_queue_thread_, log_writer_service_, logging_configuration_manager_, sensors_configuration_manager_,
        tool_id_, MakeDeviceIdProvider(canbus_com_service_)
    );
    if(!log_writer_supervisor_->Initialize()) {
        LOG_ERR("Failed to initialize the log writer supervisor.");
        return false;
    }

    if(canbus_com_service_ == nullptr) {
        LOG_INF("No CAN COM service: logging runs on this unit only, without the LOGGING command or a shared status.");
        logging_control_ = std::make_shared<WriterLoggingControl>(log_writer_supervisor_, nullptr, 0);
        return true;
    }

    // The network half sends this unit's requests for other units and for all of them. A peer follows
    // no status, so its monitor reports every restart itself. The control owns the monitor, and a
    // monitor that cannot register its handlers fails Initialize().
    network_logging_control_ = std::make_shared<RemoteLoggingControl>(
        config_work_queue_thread_, std::make_shared<CanbusComLoggingCommandSender>(canbus_com_service_),
        std::make_shared<CanbusComLoggingRestartMonitor>(canbus_com_service_, false),
        canbus_com_service_->GetNetworkInfo(), RemoteLoggingControl::Mode::Peer
    );
    if(!network_logging_control_->Initialize()) {
        LOG_ERR("Failed to initialize the network half of the logging control.");
        return false;
    }
    logging_control_ = std::make_shared<WriterLoggingControl>(
        log_writer_supervisor_, network_logging_control_, GetUnitUid()
    );

    // The handler before the exporter: the exporter shares the first status as soon as it exists,
    // and a command that arrives with no handler is dropped without an answer. Both after the
    // supervisor's Initialize(), which a request needs.
    logging_command_handler_ = std::make_unique<LoggingCommandHandler>(canbus_com_service_, logging_control_);

    // The LOGGING feature on CDMP follows the log volume: senders skip a unit that cannot log.
    try {
        logging_availability_reporter_ = std::make_unique<CanbusComLoggingAvailabilityReporter>(
            config_work_queue_thread_, sd_fs_service_, canbus_com_service_
        );
    } catch(const std::exception& e) {
        LOG_ERR("Failed to report the log volume's availability: %s", e.what());
    }

    // Not fatal: the unit still logs and takes commands; the other units just see no status from it.
    try {
        logging_status_exporter_ = std::make_unique<LoggingStatusExporter>(canbus_com_service_);
    } catch(const std::exception& e) {
        LOG_ERR("Failed to share the logging status: %s", e.what());
    }

    return true;
}

std::shared_ptr<IService> LoggingController::GetDependentService() const {
    return log_writer_supervisor_ != nullptr ? log_writer_supervisor_->GetDependentService() : nullptr;
}

#else

bool LoggingController::BuildRole() {
    // The control owns the monitor. The monitor's constructor throws when the COM service refuses a
    // handler, which fails Initialize(): the control would miss a restart that the status does not
    // show.
    remote_logging_control_ = std::make_shared<RemoteLoggingControl>(
        config_work_queue_thread_, std::make_shared<CanbusComLoggingCommandSender>(canbus_com_service_),
        std::make_shared<CanbusComLoggingRestartMonitor>(canbus_com_service_), canbus_com_service_->GetNetworkInfo(),
        RemoteLoggingControl::Mode::Remote
    );
    if(!remote_logging_control_->Initialize()) {
        LOG_ERR("Failed to initialize the remote logging control.");
        return false;
    }
    logging_control_ = remote_logging_control_;

    // The gauge answers the LOGGING command too: a stop from another unit drops its wanted start, so
    // no re-assertion undoes it (L9). After the control's Initialize(), which the command needs.
    logging_command_handler_ = std::make_unique<LoggingCommandHandler>(canbus_com_service_, logging_control_);

    // After the control subscribed: the importer's first status (Unavailable) has no predecessor to
    // be told from. It throws when the transport refuses it, which fails Initialize(): the remote
    // role is blind without the status.
    logging_status_importer_ = std::make_unique<LoggingStatusImporter>(canbus_com_service_);

    return true;
}

#endif

std::unique_ptr<LoggingTriggerSet> LoggingController::BuildTriggerSet(LoggingTriggerSet::FirstValue first_value) const {
    // Without a stored configuration (a unit whose file system cannot be written) the set has no
    // triggers, so a caller never meets a null set.
    const auto configuration = logging_control_configuration_manager_->Get();
    if(configuration == nullptr)
        LOG_WRN("No logging control configuration is available; no triggers.");

    const std::span<const LoggingTrigger> triggers = configuration != nullptr
        ? std::span<const LoggingTrigger>(configuration->triggers)
        : std::span<const LoggingTrigger>();

    auto set = std::make_unique<LoggingTriggerSet>(
        logging_control_, triggers, MakeSelectorResolver(sensors_configuration_manager_), first_value
    );

    LOG_INF(
        "Logging triggers: %u of %u subscribed.", static_cast<unsigned>(set->GetSubscribedCount()),
        static_cast<unsigned>(triggers.size())
    );

    return set;
}

// A configuration without triggers replays nothing, so a gauge does not re-render its widgets for it.
void LoggingController::ReplayFor(const LoggingTriggerSet& set) const {
    if(sensor_events_publisher_ != nullptr && set.GetSubscribedCount() > 0)
        sensor_events_publisher_->Replay();
}

// Configuration work queue only: a store from another thread has its handler queue this behind it.
void LoggingController::RebuildTriggers() {
    // Under the lock from the reset to the end of the build: an update's report either comes first
    // (the flag is set, so the new set is built with baselines) or finds the new set and switches it.
    // A set rebuilt after an update always takes baselines.
    ScopedMutex lock(trigger_set_lock_);

    // The old set goes first, so two sets never act on one change and the new one starts from
    // "unknown", which is what the replay below is for: it makes the baseline current.
    logging_trigger_set_.reset();

    try {
        logging_trigger_set_ = BuildTriggerSet(
            is_configuration_updated_ ? LoggingTriggerSet::FirstValue::IsBaseline
                                      : LoggingTriggerSet::FirstValue::OneStarts
        );
    } catch(const std::exception& e) {
        LOG_ERR(
            "Failed to rebuild the logging triggers; none act until the configuration is stored again: %s", e.what()
        );
        return;
    }

    ReplayFor(*logging_trigger_set_);
}

// The update stop's handler: on the thread that stored the configuration, before the stop is
// requested and before the store's own handler queues its work.
void LoggingController::OnConfigurationUpdated(ConfigurationService::Type type) {
    // Before the lock: a rebuild that takes it first still sees the flag, one that comes after builds
    // its set with baselines.
    is_configuration_updated_ = true;

    ScopedMutex lock(trigger_set_lock_);

    // A failed rebuild leaves no set; the next one is built with baselines.
    if(logging_trigger_set_ == nullptr)
        return;

    // Only a sensors update can take a sensor away.
    logging_trigger_set_->OnConfigurationUpdated(
        type == ConfigurationService::Type::Sensors ? MakeSelectorResolver(sensors_configuration_manager_) : nullptr
    );
}

void LoggingController::RegisterAssets() {
#if defined(CONFIG_EERIE_LEAP_ASSETS) && defined(CONFIG_EERIE_LEAP_DOMAIN_LOGGING)
    using namespace eerie_leap::subsys::assets;

    if(assets_manager_ == nullptr || sd_fs_service_ == nullptr || log_writer_service_ == nullptr)
        return;

    try {
        AssetProvider provider;
        provider.kind = AssetKind::LOGS;
        provider.name = "logs";
        provider.store = std::make_shared<AssetStore>(sd_fs_service_, LogFileNaming::LOGS_DIR);
        provider.extensions = LOG_EXTENSIONS;
        provider.access = ToMask(AssetAccess::READ) | ToMask(AssetAccess::DELETE);
        // The logs are the writer's: no uploads.
        provider.max_size = 0;

        // The file being written is neither read nor deleted. The writer can outlive the controller, as
        // the dependent service SensorsController keeps holds it, so it is held weakly.
        provider.is_in_use = [weak_writer = std::weak_ptr<LogWriterService>(log_writer_service_),
                                 store = provider.store](AssetAccess operation, std::string_view name) {
            if(operation != AssetAccess::READ && operation != AssetAccess::DELETE)
                return false;

            const auto writer = weak_writer.lock();

            return writer != nullptr && writer->IsOpenFile(store->GetPath(name));
        };

        is_assets_registered_ = assets_manager_->Register(std::move(provider));
    } catch(const std::exception& e) {
        LOG_ERR("Failed to register the logs as assets: %s", e.what());
    }
#endif
}

void LoggingController::UnregisterAssets() {
#if defined(CONFIG_EERIE_LEAP_ASSETS) && defined(CONFIG_EERIE_LEAP_DOMAIN_LOGGING)
    if(assets_manager_ != nullptr && is_assets_registered_)
        assets_manager_->Unregister(eerie_leap::subsys::assets::AssetKind::LOGS);
#endif

    is_assets_registered_ = false;
}

#ifdef CONFIG_EERIE_LEAP_DOMAIN_LOGGING

// Through the supervisor, so the wanted state is cleared and the dependent that SensorsController
// holds does not start the writer again at the next sensors restart; then waits until the
// configuration work queue has run the stop, so the writer is never dropped while it runs. Dropping
// a running LogWriterService is unsafe: its delayable tick would outlive it.
void LoggingController::StopLogWriter() {
    if(log_writer_supervisor_ == nullptr || log_writer_service_ == nullptr)
        return;

    // The supervisor itself: this stops this unit's writer, not the other units.
    log_writer_supervisor_->RequestStop(ILoggingControl::kAllUnits);

    if(config_work_queue_thread_->IsCurrentThread()) {
        // The stop is queued behind this very job, so waiting for it would wait for itself.
        LOG_ERR(
            "The logging controller is destroyed on the configuration work queue; the log writer is stopped without "
            "waiting for it."
        );
    } else {
        // A stack semaphore: the wait has no timeout, and the job gives it before this returns.
        k_sem done;
        k_sem_init(&done, 0, 1);

        try {
            // Queued behind the supervisor's run that the request just queued.
            config_work_queue_thread_->Run([&done] {
                k_sem_give(&done);
            });
            k_sem_take(&done, K_FOREVER);
        } catch(const std::exception& e) {
            LOG_ERR("Failed to wait for logging to stop: %s", e.what());
        }
    }

    // The queue did not run the stop (it no longer runs, or this is its thread): nothing else can
    // start the writer now, and a service in the middle of a transition makes Stop() wait for it.
    if(!log_writer_service_->IsStopped()) {
        LOG_WRN("The log writer is stopped directly as the logging controller goes.");

        try {
            log_writer_service_->Stop();
        } catch(const std::exception& e) {
            LOG_ERR("Failed to stop the log writer: %s", e.what());
        }
    }

    if(!log_writer_service_->IsStopped())
        LOG_ERR("The log writer still runs as the logging controller goes.");
}

#endif

// Also the failed-Initialize() path and the destructor, so each step tolerates a member that was
// never built, and the whole can run again.
void LoggingController::Teardown() {
    // The log files are not served any more, so no request asks the writer about a file past this.
    UnregisterAssets();

    // 0. No report runs after this: a store stops nothing, and the update handler's `this` is not
    //    used again. A report in its body is waited for.
    configuration_update_logging_stop_.reset();

    // 1. No rebuild can be queued any more: a handler already called on the storing thread becomes
    //    a no-op, and one in its body is waited for.
    guard_->Detach();
    if(is_handler_registered_) {
        logging_control_configuration_manager_->RegisterConfigurationUpdatedHandler(nullptr);
        is_handler_registered_ = false;
    }

    // 2. A pending rebuild is dropped, one in progress waited for.
    if(rebuild_task_.has_value()) {
        rebuild_task_->Cancel();
        rebuild_task_.reset();
    }

    // 3. The inputs: nothing requests from here on. Each waits for a handler in its body.
    {
        ScopedMutex lock(trigger_set_lock_);
        logging_trigger_set_.reset();
    }
    logging_command_handler_.reset();
#ifdef CONFIG_EERIE_LEAP_DOMAIN_LOGGING

    // 4. Logging stops while the supervisor can still do it.
    StopLogWriter();

    // 5. The record goes: the other units see Unavailable. They may never see the Stopped, which is
    //    published asynchronously and can still be on its way to the bus thread.
    logging_status_exporter_.reset();
    logging_availability_reporter_.reset();

    // 6. The control, then what it drives. The dependent that SensorsController keeps holds the
    //    stopped writer and the wanted state, never this.
    logging_control_.reset();
    network_logging_control_.reset();
    log_writer_supervisor_.reset();
    log_writer_service_.reset();
    logging_configuration_manager_.reset();
#else
    // 4. No more statuses; one already queued reaches the control's detached guard or its live handler.
    logging_status_importer_.reset();

    // 5. Waits for a send in progress. The CDMP transaction's callback holds only a flag. The control
    //    clears the restart monitor's handler, and the monitor, which it owns, unregisters from the
    //    COM service and waits for a handler in progress: not on the CDMP work queue.
    logging_control_.reset();
    remote_logging_control_.reset();
#endif

    logging_control_configuration_manager_.reset();
}

} // namespace eerie_leap::controllers
