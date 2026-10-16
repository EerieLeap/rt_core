#pragma once

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "controllers/logging_controller.h"
#include "controllers/sensors_controller.h"

#include "utilities/memory/memory_resource_manager.h"
#include "configuration/cbor/cbor_sensors_config/cbor_sensors_config.h"
#include "configuration/services/cbor_configuration_service.h"
#include "subsys/device_tree/dt_fs.h"
#include "subsys/event_bus/event_binding_subscription.h"
#include "subsys/fs/services/fs_service.h"
#include "subsys/fs/services/i_fs_service.h"
#include "subsys/time/boot_elapsed_time_provider.h"
#include "subsys/time/rtc_provider.h"
#include "subsys/time/time_service.h"

#include "domain/canbus_com_domain/commands/canbus_com_command_code.h"
#include "domain/canbus_com_domain/states/canbus_com_state_type.h"
#include "configuration/cbor/cbor_adc_config/cbor_adc_config.h"
#include "configuration/cbor/cbor_logging_control_config/cbor_logging_control_config.h"
#include "subsys/adc/i_adc_manager.h"
#include "domain/adc_domain/configuration/adc_configuration_manager.h"
#include "domain/configuration_domain/services/configuration_service.h"
#include "domain/logging_domain/control/models/logging_control_configuration.h"
#include "domain/sensor_domain/models/indicator_input_mode.h"
#include "domain/sensor_domain/models/reading_source.h"
#include "domain/sensor_domain/models/reading_status.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/sensor_domain/models/sensor_type.h"

#ifdef CONFIG_EERIE_LEAP_DOMAIN_LOGGING
#include "domain/logging_domain/models/logging_configuration.h"
#include "domain/logging_domain/models/sensor_logging_configuration.h"
#endif

#include "canbus_com_loopback_harness.h"
#include "fake_gpio.h"
#include "logging_control_test_support.h"
#include "logging_restart_test_support.h"

// The rig of the logging controller suites. Like logging_control_test_support.h, everything has
// external linkage, so the controller suite shares the one bus and configuration queue with the other
// suites of the binary; only one of logging_controller_remote.cpp and logging_controller_writer.cpp
// is in each scenario.
//
// Nothing here asserts on a thread other than the test's: jobs and handlers record into contexts
// owned by shared_ptr, every wait has a timeout, and a case that an assertion aborted leaves
// nothing a handler could reach. Every case builds its own controllers with their own
// ConfigurationService, because ConfigurationService keeps the first manager registered for a type.
namespace logging_controller_test {

using namespace logging_control_test;
using namespace canbus_com_loopback_test;

using eerie_leap::controllers::LoggingController;
using eerie_leap::controllers::SensorsController;
using eerie_leap::subsys::adc::AdcChannelHandle;
using eerie_leap::subsys::adc::IAdc;
using eerie_leap::subsys::adc::IAdcManager;
using eerie_leap::domain::adc_domain::configuration::AdcConfigurationManager;
using eerie_leap::domain::canbus_com_domain::commands::CanbusComCommandCode;
using eerie_leap::domain::canbus_com_domain::states::CanbusComStateType;
using eerie_leap::domain::configuration_domain::services::ConfigurationService;
using eerie_leap::domain::logging_domain::control::configuration::LoggingControlConfigurationManager;
using eerie_leap::domain::logging_domain::control::models::LoggingControlConfiguration;
using eerie_leap::domain::sensor_domain::configuration::SensorsConfigurationManager;
using eerie_leap::domain::sensor_domain::models::IndicatorInputMode;
using eerie_leap::domain::sensor_domain::models::ReadingSource;
using eerie_leap::domain::sensor_domain::models::ReadingStatus;
using eerie_leap::domain::sensor_domain::models::Sensor;
using eerie_leap::domain::sensor_domain::models::SensorReading;
using eerie_leap::domain::sensor_domain::models::SensorType;
using eerie_leap::domain::sensor_domain::services::SensorEventsPublisher;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;
using eerie_leap::subsys::device_tree::DtFs;
using eerie_leap::subsys::event_bus::EventBinding;
using eerie_leap::subsys::event_bus::SubscribeEventBinding;
using eerie_leap::subsys::fs::services::DirectoryEntry;
using eerie_leap::subsys::fs::services::FsService;
using eerie_leap::subsys::fs::services::IFsService;
using eerie_leap::subsys::time::BootElapsedTimeProvider;
using eerie_leap::subsys::time::RtcProvider;
using eerie_leap::subsys::time::TimeService;
using eerie_leap::configuration::services::CborConfigurationService;
using fake_gpio_test::FakeGpio;

inline constexpr uint8_t LOGGING_CODE = std::to_underlying(CanbusComCommandCode::LOGGING);
inline constexpr uint8_t LOGGING_STATUS_CODE = std::to_underlying(CanbusComStateType::LOGGING_STATUS);

inline constexpr const char* kControllerToolId = "rt_core_controller";

// The transaction IDs of the peer's commands start here, so none equals one the unit uses (they start
// at 1): the unit hears its own broadcasts on the loopback bus and answers them with its own IDs.
inline constexpr uint8_t kPeerFirstTransaction = 0x80;

// Stored LoggingControl configuration {Sensors, DataUpdated, Value, SensorId "ab"}, as a client
// sends it over SMP (the golden bytes of the configuration suites).
inline const std::vector<uint8_t> k_trigger_on_ab = {
    0x9F, 0x9F, 0x9F, 0x01, 0x00, 0x01, 0xF5, 0x00, 0x62, 0x61, 0x62, 0xFF, 0xFF, 0xFF};

// Polls @p predicate every 2 ms until it holds or @p timeout_ms passed.
template<typename TPredicate>
[[nodiscard]] bool WaitUntil(TPredicate predicate, int timeout_ms = k_wait_ms) {
    const int64_t deadline = k_uptime_get() + timeout_ms;
    while(!predicate()) {
        if(k_uptime_get() >= deadline)
            return false;

        k_msleep(2);
    }

    return true;
}

// ======== Triggers ========

// A stored configuration with a trigger on each of @p sensor_ids.
inline LoggingControlConfiguration TriggersOn(std::initializer_list<std::string_view> sensor_ids) {
    LoggingControlConfiguration configuration(std::allocator_arg, eerie_leap::utilities::memory::Mrm::GetDefaultPmr());
    for(const auto sensor_id : sensor_ids)
        configuration.triggers.push_back(SensorTrigger(sensor_id));

    return configuration;
}

// The subscription generation of sensor_events: it moves on every subscribe and unsubscribe, so it
// tells when a trigger set was built or replaced.
inline uint32_t SensorSubscriptionGeneration() {
    return SensorEventsChannel::GetInstance().GetSubscriptionGeneration();
}

// ======== The unit ========

struct SensorSpec {
    std::string_view id;
    bool is_enabled = true;
};

// What the controller is built on: the internal file system, the sensors configuration the
// resolver reads, the frame and a running sensor events publisher over it. VIRTUAL_INDICATOR
// sensors: the pipeline is not running, so Commit() stands in for it.
class ControllerRig {
public:
    std::shared_ptr<FsService> fs_service;
    std::shared_ptr<SensorsConfigurationManager> sensors_manager;
    std::shared_ptr<SensorReadingsFrame> frame = std::make_shared<SensorReadingsFrame>();
    std::shared_ptr<TimeService> time_service;
    std::shared_ptr<SensorEventsPublisher> publisher;

    // @param is_fs_formatted A Harness formatted the file system already.
    ControllerRig(std::initializer_list<SensorSpec> specs, bool is_fs_formatted) {
        DtFs::InitInternalFs();
        auto* mount_point = DtFs::GetInternalFsMp();
        zassert_not_null(mount_point, "The overlay has no intfs0 mount");

        fs_service = std::make_shared<FsService>(mount_point);
        if(!is_fs_formatted)
            zassert_true(fs_service->Format(), "The internal file system could not be formatted");

        time_service = std::make_shared<TimeService>(
            std::make_shared<BootElapsedTimeProvider>(), std::make_shared<RtcProvider>());

        sensors_manager = std::make_shared<SensorsConfigurationManager>(
            std::make_unique<CborConfigurationService<CborSensorsConfig>>("sensors_config", fs_service, TestConfigQueue()),
            nullptr, 0, 0);

        std::vector<std::shared_ptr<Sensor>> sensors;
        for(const auto& spec : specs) {
            auto sensor = std::make_shared<Sensor>(
                std::allocator_arg, eerie_leap::utilities::memory::Mrm::GetDefaultPmr(), spec.id);
            sensor->is_enabled = spec.is_enabled;
            sensor->configuration.type = SensorType::VIRTUAL_INDICATOR;
            sensor->configuration.expression = "1 > 0";
            sensor->configuration.sampling_rate_ms = 1000;
            sensors.push_back(std::move(sensor));
        }
        zassert_true(sensors_manager->Update(sensors), "The sensors were not stored");

        // The frame is laid out for the stored sensors, which are the ones the resolver sees.
        frame->Configure(*sensors_manager->Get());

        publisher = std::make_shared<SensorEventsPublisher>(frame);
        zassert_true(publisher->Initialize(), "The publisher did not initialize");
        zassert_true(publisher->Start(), "The publisher did not start");
    }

    // After every controller built on it.
    ~ControllerRig() {
        publisher->Stop();
    }

    ControllerRig(const ControllerRig&) = delete;
    ControllerRig& operator=(const ControllerRig&) = delete;

    // Commits a processed value, as the pipeline does; the next publisher tick takes it.
    void Commit(std::string_view sensor_id, float value) {
        for(const auto& sensor : *sensors_manager->Get()) {
            if(sensor->id != sensor_id)
                continue;

            SensorReading reading(sensor.get());
            reading.source = ReadingSource::PROCESSING;
            reading.status = ReadingStatus::PROCESSED;
            reading.value = value;
            reading.timestamp = time_service->GetCurrentTime();
            frame->AddOrUpdateReading(reading);

            return;
        }

        zassert_unreachable("The rig has no sensor %s", std::string(sensor_id).c_str());
    }
};

// The setup hook that stores triggers on @p sensor_ids before the controller builds its set, as the
// gauge stores its button's.
inline LoggingController::ConfigurationSetup StoresTriggersOn(std::initializer_list<std::string_view> sensor_ids) {
    std::vector<std::string> ids;
    for(const auto sensor_id : sensor_ids)
        ids.emplace_back(sensor_id);

    return [ids = std::move(ids)](std::shared_ptr<LoggingControlConfigurationManager> manager) {
        LoggingControlConfiguration configuration(
            std::allocator_arg, eerie_leap::utilities::memory::Mrm::GetDefaultPmr());
        for(const auto& sensor_id : ids)
            configuration.triggers.push_back(SensorTrigger(sensor_id));

        zassert_true(manager->Update(configuration), "The hook's triggers were not stored");
    };
}

// A manager that stands in for a type whose domain the suite does not build (Ui, Display, and System
// where the logging domain is off): the logging control's manager on a file of its own, registered
// under @p type. A store goes through the same base as the real one's.
inline std::shared_ptr<LoggingControlConfigurationManager> MakeStandIn(
    ConfigurationService& service,
    ConfigurationService::Type type,
    const char* name,
    const std::shared_ptr<IFsService>& fs_service) {

    auto manager = std::make_shared<LoggingControlConfigurationManager>(
        std::make_unique<CborConfigurationService<CborLoggingControlConfig>>(name, fs_service, TestConfigQueue()));
    service.RegisterCborConfigurationManager(type, manager);

    return manager;
}

// A configuration update of a stand-in, stored on the calling thread.
inline void StoreStandIn(const std::shared_ptr<LoggingControlConfigurationManager>& manager) {
    zassert_true(manager->Update(TriggersOn({})), "The stand-in's configuration was not stored");
}

// An internal file system that cannot be read or written, as a unit whose flash failed has it: no
// configuration loads or stores, so a manager on it has no configuration.
class UnwritableFsService final : public IFsService {
public:
    bool Initialize() override { return true; }
    bool IsAvailable() const override { return true; }
    bool WriteFile(std::string_view, const void*, size_t, bool) override { return false; }
    bool ReadFile(std::string_view, void*, size_t, size_t&) override { return false; }
    int OpenFile(std::string_view, fs_mode_t, fs_file_t*) override { return -ENOTSUP; }
    bool CreateDirectory(std::string_view) override { return false; }
    bool Exists(std::string_view) const override { return false; }
    bool DeleteFile(std::string_view) override { return false; }
    bool DeleteRecursive(std::string_view) override { return false; }
    std::vector<std::string> ListFiles(std::string_view) const override { return {}; }
    int ListDirectory(std::string_view, size_t, size_t, std::vector<DirectoryEntry>& entries, size_t& count) const override {
        entries.clear();
        count = 0;

        return -ENODEV;
    }
    std::optional<size_t> GetFileSize(std::string_view) const override { return std::nullopt; }
    uint64_t GetTotalSpace() const override { return 0; }
    uint64_t GetUsedSpace() const override { return 0; }
    bool Format() override { return false; }
};

// What a controller of a case is given beyond the rig. A null configuration service, or a false
// has_* flag, leaves that dependency out.
struct Parts {
    std::shared_ptr<CanbusComService> com;
    std::shared_ptr<ConfigurationService> configuration_service = std::make_shared<ConfigurationService>();
    // For a ProcessingRig: whether the controller uses configuration_service instead of the rig's,
    // which SensorsController and the ADC manager are registered with. A null one is then its own too.
    bool uses_own_service = false;
    std::shared_ptr<WorkQueueThread> queue = TestConfigQueue();
    // The controller's internal file system; null: the rig's. The sensors and the log volume stay on the rig's.
    std::shared_ptr<IFsService> fs_service;
    bool has_sensors_manager = true;
    bool has_publisher = true;
    bool has_log_volume = true;
    bool has_time_service = true;
    bool has_frame = true;
};

// The controller of whichever role the binary is built for: the remote role ignores what only the
// writer uses.
inline std::unique_ptr<LoggingController> MakeController(ControllerRig& rig, Parts parts) {
    return std::make_unique<LoggingController>(
        parts.fs_service != nullptr ? std::move(parts.fs_service) : rig.fs_service,
        std::move(parts.queue),
        std::move(parts.configuration_service),
        parts.has_sensors_manager ? rig.sensors_manager : nullptr,
        std::move(parts.com),
        parts.has_publisher ? rig.publisher : nullptr,
        parts.has_log_volume ? rig.fs_service : nullptr,
        parts.has_time_service ? rig.time_service : nullptr,
        parts.has_frame ? rig.frame : nullptr,
        kControllerToolId);
}

// ======== What a trigger or a store does, seen from outside ========

// The values the publisher delivers for one sensor, as a subscriber of sensor_events sees them. A
// value shows that the publisher took a commit, and a second one for the same commit can only be a
// replay.
class SensorProbe {
private:
    struct Context {
        k_mutex lock;
        k_sem received;
        std::deque<float> values;

        Context() {
            k_mutex_init(&lock);
            k_sem_init(&received, 0, K_SEM_MAX_LIMIT);
        }
    };

    std::shared_ptr<Context> context_ = std::make_shared<Context>();
    AnySubscription subscription_;

public:
    explicit SensorProbe(std::string_view sensor_id) {
        subscription_ = SubscribeEventBinding(
            SensorTrigger(sensor_id).binding,
            [context = context_](const EventData& value) {
                const auto* number = std::get_if<float>(&value);
                if(number == nullptr)
                    return;

                {
                    ScopedMutex guard(context->lock);
                    context->values.push_back(*number);
                }
                k_sem_give(&context->received);
            });
        zassert_not_null(subscription_.get(), "The probe's subscription was refused.");
    }

    // The next delivery of @p expected, skipping other values; each call consumes what it reads.
    [[nodiscard]] bool WaitForValue(float expected, int timeout_ms = k_wait_ms) {
        const int64_t deadline = k_uptime_get() + timeout_ms;

        while(true) {
            const int64_t remaining = deadline - k_uptime_get();
            if(remaining < 0 || k_sem_take(&context_->received, Ms(static_cast<int>(remaining))) != 0)
                return false;

            float value = 0.0F;
            {
                ScopedMutex guard(context_->lock);
                value = context_->values.front();
                context_->values.pop_front();
            }

            if(value == expected)
                return true;
        }
    }

    // Forgets what was delivered and not yet consumed, so the next wait is for a new delivery.
    void Clear() {
        ScopedMutex guard(context_->lock);

        context_->values.clear();
        k_sem_reset(&context_->received);
    }
};

// A job on the configuration work queue that gives a semaphore: the queue is past everything queued
// before it. Same as FlushQueue(), named for the controller cases.
[[nodiscard]] inline bool Flush() {
    return FlushQueue(*TestConfigQueue());
}

// ======== Real sensor processing ========

using SensorList = std::vector<std::shared_ptr<Sensor>>;

// A VIRTUAL_INDICATOR read every 100 ms, in Level mode: "1" or "0" is a constant switch.
inline std::shared_ptr<Sensor> Indicator(std::string_view id, const char* expression, bool is_enabled = true) {
    auto sensor = std::make_shared<Sensor>(
        std::allocator_arg, eerie_leap::utilities::memory::Mrm::GetDefaultPmr(), id);
    sensor->is_enabled = is_enabled;
    sensor->configuration.type = SensorType::VIRTUAL_INDICATOR;
    sensor->configuration.expression = expression;
    sensor->configuration.sampling_rate_ms = 100;

    return sensor;
}

// A sensor no trigger binds to.
inline std::shared_ptr<Sensor> Analog(std::string_view id, const char* expression = "2 + 2", bool is_enabled = true) {
    auto sensor = std::make_shared<Sensor>(
        std::allocator_arg, eerie_leap::utilities::memory::Mrm::GetDefaultPmr(), id);
    sensor->is_enabled = is_enabled;
    sensor->configuration.type = SensorType::VIRTUAL_ANALOG;
    sensor->configuration.expression = expression;
    sensor->configuration.sampling_rate_ms = 100;

    return sensor;
}

// An input on a GPIO channel. Processing needs a GPIO for it; with a FakeGpio that refuses
// registrations it cannot start.
inline std::shared_ptr<Sensor> Input(std::string_view id, int channel) {
    auto sensor = std::make_shared<Sensor>(
        std::allocator_arg, eerie_leap::utilities::memory::Mrm::GetDefaultPmr(), id);
    sensor->configuration.type = SensorType::PHYSICAL_INDICATOR;
    sensor->configuration.channel = channel;

    return sensor;
}

// The gauge's logging button, as SetupTestLoggingButton() in its main.cpp stores it.
inline std::shared_ptr<Sensor> Button(std::string_view id = "logging_button", int channel = 0) {
    auto sensor = Input(id, channel);
    sensor->configuration.debounce_ms = 30;
    sensor->configuration.input_mode = IndicatorInputMode::Toggle;

    return sensor;
}

// An ADC manager that only counts channels: the ADC configuration manager reads nothing else.
class FakeAdcManager final : public IAdcManager {
public:
    bool Initialize() override { return true; }
    [[nodiscard]] int GetAdcCount() const override { return 1; }
    [[nodiscard]] int GetChannelCount() const override { return 2; }
    [[nodiscard]] std::span<const std::shared_ptr<IAdc>> GetAdcs() const override { return {}; }
    AdcChannelHandle Resolve(int) override { throw std::invalid_argument("The fake ADC manager has no channels."); }
};

// What the controller is built on when a case applies sensors configurations: a SensorsController
// over the internal file system and the test configuration queue, as the gauge builds it (no
// configuration service, CAN or ADC; a GPIO only where a case needs an input), and a running sensor
// events publisher over its frame. The controller's resolver reads the controller's own manager, so an
// apply is seen by the logging controller, and the controller's dependent is registered with it.
// Sensors are VIRTUAL_INDICATOR or VIRTUAL_ANALOG ones read by the real pipeline.
class ProcessingRig {
public:
    std::shared_ptr<FsService> fs_service;
    std::shared_ptr<TimeService> time_service;
    std::shared_ptr<SensorReadingsFrame> frame = std::make_shared<SensorReadingsFrame>();
    std::shared_ptr<FakeGpio> gpio;
    // The unit's: SensorsController registers the sensors with it, and the rig the ADC configuration
    // under Type::Adc. A controller built by MakeController() uses it.
    std::shared_ptr<ConfigurationService> configuration_service = std::make_shared<ConfigurationService>();
    // Over a fake ADC manager and given to SensorsController, so a store restarts processing as the
    // ADC controller's would; no analog sensor is read.
    std::shared_ptr<AdcConfigurationManager> adc_configuration_manager;
    std::unique_ptr<SensorsController> sensors_controller;
    std::shared_ptr<SensorsConfigurationManager> sensors_manager;
    std::shared_ptr<SensorEventsPublisher> publisher;

    /**
     * @param first_sensors Stored before the controller's updated handler exists, so they are no
     *        restart of their own.
     * @param is_fs_formatted A Harness formatted the file system already.
     * @param hook Runs in SensorsController::Initialize() after @p first_sensors are stored, as the
     *        gauge's hooks do (they run before the logging controller exists).
     */
    ProcessingRig(
        const SensorList& first_sensors,
        bool is_fs_formatted,
        std::shared_ptr<FakeGpio> gpio_to_use = nullptr,
        SensorsController::ConfigurationSetup hook = nullptr)
            : gpio(std::move(gpio_to_use)) {

        DtFs::InitInternalFs();
        auto* mount_point = DtFs::GetInternalFsMp();
        zassert_not_null(mount_point, "The overlay has no intfs0 mount");

        fs_service = std::make_shared<FsService>(mount_point);
        if(!is_fs_formatted)
            zassert_true(fs_service->Format(), "The internal file system could not be formatted");

        time_service = std::make_shared<TimeService>(
            std::make_shared<BootElapsedTimeProvider>(), std::make_shared<RtcProvider>());

        adc_configuration_manager = std::make_shared<AdcConfigurationManager>(
            std::make_unique<CborConfigurationService<CborAdcConfig>>("adc_config", fs_service, TestConfigQueue()),
            std::make_shared<FakeAdcManager>());
        configuration_service->RegisterCborConfigurationManager(
            ConfigurationService::Type::Adc, adc_configuration_manager);

        sensors_controller = std::make_unique<SensorsController>(
            fs_service, TestConfigQueue(), configuration_service, time_service, frame, nullptr, gpio,
            nullptr, adc_configuration_manager);
        zassert_equal(sensors_controller->Initialize([&first_sensors, &hook](auto manager) {
            zassert_true(manager->Update(first_sensors), "The sensors were not stored");
            if(hook)
                hook(manager);
        }), 0, "The sensors controller initializes");
        sensors_manager = sensors_controller->GetConfigurationManager();

        publisher = std::make_shared<SensorEventsPublisher>(frame);
        zassert_true(publisher->Initialize(), "The publisher did not initialize");
        zassert_true(publisher->Start(), "The publisher did not start");
    }

    // After every controller built on it.
    ~ProcessingRig() {
        sensors_controller->GetProcessingService()->Stop();
        publisher->Stop();
    }

    ProcessingRig(const ProcessingRig&) = delete;
    ProcessingRig& operator=(const ProcessingRig&) = delete;

#ifdef CONFIG_EERIE_LEAP_DOMAIN_LOGGING
    // Before StartProcessing(), once the controller is initialized: the writer, as the composition
    // root registers it, so a restart of processing stops and starts it.
    void Register(const LoggingController& controller) {
        zassert_not_null(controller.GetDependentService().get(), "The controller has its dependent");
        sensors_controller->RegisterDependentService(controller.GetDependentService());
    }
#endif

    void StartProcessing() {
        zassert_equal(sensors_controller->Start(), 0, "Processing starts");
    }

    [[nodiscard]] bool IsProcessingStopped() const {
        return sensors_controller->GetProcessingService()->IsStopped();
    }

    // Commits a processed value for a sensor that is stored, as the pipeline does, without waiting for
    // the next publisher tick.
    void Commit(std::string_view sensor_id, float value) {
        for(const auto& sensor : *sensors_manager->Get()) {
            if(sensor->id != sensor_id)
                continue;

            SensorReading reading(sensor.get());
            reading.source = ReadingSource::PROCESSING;
            reading.status = ReadingStatus::PROCESSED;
            reading.value = value;
            reading.timestamp = time_service->GetCurrentTime();
            frame->AddOrUpdateReading(reading);

            return;
        }

        zassert_unreachable("The rig has no sensor %s", std::string(sensor_id).c_str());
    }

    // Stores @p sensors as an SMP apply does and returns once the restart it causes has run on the
    // configuration work queue and what that queued (a request, a send). The updated handler queued
    // the restart before Update() returned, so the first flush returns after it.
    void Apply(const SensorList& sensors) {
        zassert_true(sensors_manager->Update(sensors), "The sensors were not stored");
        zassert_true(Flush());
        zassert_true(Flush());
    }
};

// The controller of whichever role the binary is built for, on real processing, with the rig's
// configuration service unless the case gives its own.
inline std::unique_ptr<LoggingController> MakeController(ProcessingRig& rig, Parts parts) {
    return std::make_unique<LoggingController>(
        parts.fs_service != nullptr ? std::move(parts.fs_service) : rig.fs_service,
        std::move(parts.queue),
        parts.uses_own_service ? std::move(parts.configuration_service) : rig.configuration_service,
        parts.has_sensors_manager ? rig.sensors_manager : nullptr,
        std::move(parts.com),
        parts.has_publisher ? rig.publisher : nullptr,
        parts.has_log_volume ? rig.fs_service : nullptr,
        parts.has_time_service ? rig.time_service : nullptr,
        parts.has_frame ? rig.frame : nullptr,
        kControllerToolId);
}

// One job on the configuration queue that stores configurations through a manager (inline: the
// save runs on its own thread), gives `stored`, then holds the queue until Release(). Whatever the
// stores queued, such as the controller's rebuild, is provably pending behind it. The job holds the
// manager, never the controller, so a controller destroyed meanwhile cannot be reached through it.
class HeldStore {
private:
    struct Context {
        k_sem stored;
        k_sem release;
        std::vector<bool> results;

        Context() {
            k_sem_init(&stored, 0, 1);
            k_sem_init(&release, 0, K_SEM_MAX_LIMIT);
        }
    };

    std::shared_ptr<Context> context_ = std::make_shared<Context>();

public:
    // @param stores The sensors of the triggers of each store, in order.
    // The job holds the queue until Release(); a Release() before it runs makes it hold nothing.
    HeldStore(
        std::shared_ptr<LoggingControlConfigurationManager> manager,
        std::vector<std::vector<std::string>> stores) {

        TestConfigQueue()->Run([context = context_, manager = std::move(manager), stores = std::move(stores)] {
            for(const auto& sensor_ids : stores) {
                LoggingControlConfiguration configuration(
                    std::allocator_arg, eerie_leap::utilities::memory::Mrm::GetDefaultPmr());
                for(const auto& sensor_id : sensor_ids)
                    configuration.triggers.push_back(SensorTrigger(sensor_id));

                context->results.push_back(manager->Update(configuration));
            }

            k_sem_give(&context->stored);
            // Gives up after 5 s, so a case that never releases cannot wedge the queue for good.
            k_sem_take(&context->release, K_SECONDS(5));
        });
    }

    ~HeldStore() {
        Release();
    }

    // Returns once the stores are done and the queue is held.
    [[nodiscard]] bool WaitUntilStored(int timeout_ms = k_wait_ms) {
        return k_sem_take(&context_->stored, Ms(timeout_ms)) == 0;
    }

    void Release() {
        k_sem_give(&context_->release);
    }

    // Valid once WaitUntilStored() returned true.
    [[nodiscard]] bool AllStored() const {
        return !context_->results.empty()
            && std::ranges::all_of(context_->results, [](bool is_stored) { return is_stored; });
    }
};

// Applies CBOR to a type through the configuration service on the configuration queue, as an SMP
// apply does. The job holds the service, never the controller.
class QueuedApply {
private:
    struct Context {
        k_sem done;
        bool result = false;

        Context() {
            k_sem_init(&done, 0, 1);
        }
    };

    std::shared_ptr<Context> context_ = std::make_shared<Context>();

public:
    QueuedApply(
        std::shared_ptr<ConfigurationService> configuration_service,
        ConfigurationService::Type type,
        std::vector<uint8_t> cbor) {

        TestConfigQueue()->Run([context = context_, configuration_service = std::move(configuration_service), type, cbor = std::move(cbor)] {
            context->result = configuration_service->ApplyCborConfiguration(type, cbor);
            k_sem_give(&context->done);
        });
    }

    // Whether the apply ran and succeeded.
    [[nodiscard]] bool WaitForSuccess(int timeout_ms = k_wait_ms) {
        return k_sem_take(&context_->done, Ms(timeout_ms)) == 0 && context_->result;
    }
};

// ======== The CDMP peer ========

// The scripted peer of a Harness, as a logging unit and as a sender of the LOGGING command.
class Peer {
private:
    Harness& harness_;
    uint8_t next_version_ = 1;
    uint8_t next_transaction_ = kPeerFirstTransaction;

public:
    explicit Peer(Harness& harness) : harness_(harness) {}

    // The peer's LOGGING_STATUS record, as a logging unit shares it: {active} or {inactive, reason}.
    void ShareStatus(const Bytes& data) {
        harness_.SendAsPeer(STATE_CHANGE, NotificationFrame(PEER_ID, next_version_, LOGGING_STATUS_CODE, data));
        zassert_true(harness_.WaitForAcknowledgement(PEER_ID, next_version_), "The unit applied the record");
        next_version_++;
    }

    // The peer reboots: its ID claim again with its UID, which the unit reports as a restart, and its
    // first heartbeat with the digest of @p records. Its next record is version 1. Returns once the
    // unit has taken both; no LOGGING broadcast may come meanwhile, since nothing is sent before the
    // restarted unit can accept it. See logging_restart_test::WaitForRestartTaken() for when the
    // unit's replay request is @p is_sync_request_due.
    void Restart(std::initializer_list<Record> records, bool is_sync_request_due) {
        logging_restart_test::SendRestart(harness_, PEER_ID, PEER_UID, records);
        next_version_ = 1;
        zassert_false(logging_restart_test::WaitForRestartTaken(harness_, PEER_ID, is_sync_request_due),
            "Nothing is sent before the restarted unit can accept it");
    }

    // The unit's next LOGGING broadcast (from its remote control), or nothing in time.
    std::optional<RecordedFrame> WaitForBroadcast(int timeout_ms = k_wait_ms) {
        return harness_.bus.Wait(COMMAND, logging_restart_test::IsLoggingBroadcastFrame, timeout_ms);
    }

    // The unit's next notification of its LOGGING_STATUS record that carries @p data, skipping
    // resends of the older ones; acknowledges each, so the unit stops resending it.
    bool WaitForRecord(const Bytes& data, int timeout_ms = k_wait_ms) {
        const int64_t deadline = k_uptime_get() + timeout_ms;

        while(true) {
            const int64_t remaining = deadline - k_uptime_get();
            if(remaining < 0)
                return false;

            const auto notification = harness_.WaitForNotification(static_cast<int>(remaining));
            if(!notification.has_value())
                return false;

            if(notification->state_type != LOGGING_STATUS_CODE)
                continue;

            harness_.SendAsPeer(STATE_CHANGE_RESPONSE, AcknowledgementFrame(PEER_ID, OWN_ID, notification->version));
            if(notification->data == CdmpStateData(data))
                return true;
        }
    }

    // The unit's COMMAND_RESPONSE to a LOGGING command of transaction @p transaction, or nothing in time.
    std::optional<RecordedFrame> WaitForAnswer(uint8_t transaction, int timeout_ms = FRAME_TIMEOUT_MS) {
        return harness_.bus.Wait(
            COMMAND_RESPONSE,
            [transaction](const Bytes& bytes) {
                return bytes.size() >= 4 && bytes[0] == OWN_ID && bytes[1] == LOGGING_CODE && bytes[2] == transaction;
            },
            timeout_ms);
    }

    // The unit's answer to its own LOGGING broadcast @p broadcast: the loopback bus hands the unit its
    // own frames, which a real bus does not, so the unit's handler answers itself. Waiting for it
    // means the handler is done with the broadcast, and nothing of it is left to arrive.
    bool WaitForOwnAnswer(const RecordedFrame& broadcast, int timeout_ms = FRAME_TIMEOUT_MS) {
        if(broadcast.data.size() < 4)
            return false;

        return WaitForAnswer(broadcast.data[2], timeout_ms).has_value();
    }

    // A LOGGING command from the peer to every unit, as another unit's update stop sends it: 1 starts,
    // 0 stops. Consumes the peer's own frame from the recorder, so WaitForBroadcast() sees only the
    // unit's. The unit's result code, or nothing in time.
    std::optional<CdmpResultCode> Broadcast(const Bytes& data) {
        const uint8_t transaction = next_transaction_++;

        harness_.SendAsPeer(COMMAND, CdmpCommandRequestMessage{
            .target_device_id = CdmpDevice::DEVICE_ID_BROADCAST,
            .command_code = LOGGING_CODE,
            .transaction_id = transaction,
            .data = data}.ToCanFrame());

        if(!harness_.bus.Wait(COMMAND, [transaction](const Bytes& bytes) {
                return logging_restart_test::IsLoggingBroadcastFrame(bytes) && bytes[2] == transaction;
            }).has_value()) {
            return std::nullopt;
        }

        const auto answer = WaitForAnswer(transaction);
        if(!answer.has_value())
            return std::nullopt;

        return CdmpCommandResponseMessage::FromCanFrame(answer->data).result_code;
    }

    // A LOGGING command from the peer to the unit: 1 starts, 0 stops. The unit's result code, or
    // nothing in time.
    std::optional<CdmpResultCode> Ask(const Bytes& data) {
        const uint8_t transaction = next_transaction_++;

        harness_.SendAsPeer(COMMAND, CdmpCommandRequestMessage{
            .target_device_id = OWN_ID,
            .command_code = LOGGING_CODE,
            .transaction_id = transaction,
            .data = data}.ToCanFrame());

        const auto frame = harness_.bus.Wait(
            COMMAND_RESPONSE,
            [transaction](const Bytes& bytes) {
                return bytes.size() >= 4 && bytes[0] == OWN_ID && bytes[1] == LOGGING_CODE && bytes[2] == transaction;
            });
        if(!frame.has_value())
            return std::nullopt;

        return CdmpCommandResponseMessage::FromCanFrame(frame->data).result_code;
    }
};

// Records the StatusUpdated events in order, as a consumer of logging_events sees them.
class StatusLog {
public:
    struct Entry {
        bool is_active;
        std::optional<LoggingStatusReason> reason;
    };

private:
    struct Inbox {
        k_sem received;
        k_mutex lock;
        std::deque<Entry> entries;

        Inbox() {
            k_sem_init(&received, 0, K_SEM_MAX_LIMIT);
            k_mutex_init(&lock);
        }
    };

    std::shared_ptr<Inbox> inbox_ = std::make_shared<Inbox>();
    AnySubscription subscription_;

public:
    StatusLog() {
        subscription_ = CreateScopedSubscription(
            LoggingEventsChannel::GetInstance(),
            LoggingEventType::StatusUpdated,
            [inbox = inbox_](const LoggingEventsChannel::EventMessage& event) {
                const auto active = event.payload.find(LoggingPayloadType::IsActive);
                if(active == event.payload.end() || !std::holds_alternative<bool>(active->second))
                    return;

                Entry entry { .is_active = std::get<bool>(active->second) };
                const auto reason = event.payload.find(LoggingPayloadType::Reason);
                if(reason != event.payload.end() && std::holds_alternative<uint32_t>(reason->second))
                    entry.reason = static_cast<LoggingStatusReason>(std::get<uint32_t>(reason->second));

                {
                    ScopedMutex guard(inbox->lock);
                    inbox->entries.push_back(entry);
                }
                k_sem_give(&inbox->received);
            });
        zassert_not_null(subscription_.get(), "The status log's subscription was refused.");
    }

    std::optional<Entry> WaitForEntry(int timeout_ms = k_wait_ms) {
        if(k_sem_take(&inbox_->received, Ms(timeout_ms)) != 0)
            return std::nullopt;

        ScopedMutex guard(inbox_->lock);
        auto entry = inbox_->entries.front();
        inbox_->entries.pop_front();

        return entry;
    }
};

#ifdef CONFIG_EERIE_LEAP_DOMAIN_LOGGING

using eerie_leap::domain::logging_domain::models::LoggingConfiguration;
using eerie_leap::domain::logging_domain::models::SensorLoggingConfiguration;

// Stores a logging configuration that logs @p sensor_ids, so a file has a group, as rt_daq's would.
inline void StoreLoggingConfiguration(LoggingController& controller, std::initializer_list<std::string_view> sensor_ids) {
    LoggingConfiguration configuration(std::allocator_arg, eerie_leap::utilities::memory::Mrm::GetDefaultPmr());
    configuration.logging_interval_ms = 100;
    configuration.max_log_size_mb = 0;
    for(const auto sensor_id : sensor_ids) {
        configuration.sensor_configurations.emplace(
            eerie_leap::utilities::string::StringHelpers::GetHash(sensor_id),
            SensorLoggingConfiguration { .is_enabled = true });
    }

    zassert_true(controller.GetConfigurationManager()->Update(configuration), "The logging configuration was not stored");
}

// The bytes of the log file the writer wrote, the only one of a case; empty when there is none.
inline std::vector<uint8_t> ReadTheLogFile(FsService& fs_service) {
    if(!fs_service.Exists(CONFIG_EERIE_LEAP_LOG_DATA_FILES_DIR))
        return {};

    const auto names = fs_service.ListFiles(CONFIG_EERIE_LEAP_LOG_DATA_FILES_DIR);
    if(names.size() != 1)
        return {};

    const std::string path = std::string(CONFIG_EERIE_LEAP_LOG_DATA_FILES_DIR) + "/" + names.front();
    const auto size = fs_service.GetFileSize(path);
    if(!size.has_value())
        return {};

    std::vector<uint8_t> content(*size);
    size_t read_size = 0;
    if(!fs_service.ReadFile(path, content.data(), content.size(), read_size))
        return {};

    content.resize(read_size);

    return content;
}

inline bool ContainsText(const std::vector<uint8_t>& content, std::string_view text) {
    return std::search(content.begin(), content.end(), text.begin(), text.end()) != content.end();
}

// The log files of the data directory: one per session.
inline size_t CountLogFiles(FsService& fs_service) {
    if(!fs_service.Exists(CONFIG_EERIE_LEAP_LOG_DATA_FILES_DIR))
        return 0;

    return fs_service.ListFiles(CONFIG_EERIE_LEAP_LOG_DATA_FILES_DIR).size();
}

// A property of the header comment, as the logger writes LoggerMetadata.
inline std::string Property(const char* name, const std::string& value) {
    return std::string("<e name=\"") + name + "\">" + value + "</e>";
}

#endif // CONFIG_EERIE_LEAP_DOMAIN_LOGGING

} // namespace logging_controller_test
