// Control sensors (docs/sensors.md, "Control sensors"): CONTROL_INDICATOR and CONTROL_ANALOG sensors,
// whose input a control publishes as InputRequested on sensor_events. The cases run the processing
// service over the real event channel on every board, with the order of the queue and the loss of a
// level checked on a reader of its own, whose callback holds the first reading until the case lets go.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "configuration/cbor/cbor_sensors_config/cbor_sensors_config.h"
#include "configuration/services/cbor_configuration_service.h"
#include "domain/sensor_domain/configuration/sensors_configuration_manager.h"
#include "domain/sensor_domain/event_bus/sensor_event_bus.h"
#include "domain/sensor_domain/isr_sensor_readers/control_sensor_reader.h"
#include "domain/sensor_domain/isr_sensor_readers/isr_sensor_reader_factory.h"
#include "domain/sensor_domain/models/indicator_input_mode.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/sensor_domain/processors/i_reading_processor.h"
#include "domain/sensor_domain/runtime/sensor_pipeline_builder.h"
#include "domain/sensor_domain/runtime/sensor_runtime.h"
#include "domain/sensor_domain/services/sensor_events_publisher.h"
#include "domain/sensor_domain/services/sensors_processing_service.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "subsys/event_bus/event.h"
#include "subsys/event_bus/i_scoped_subscription.h"
#include "subsys/threading/scoped_mutex.h"
#include "subsys/threading/work_queue_thread.h"
#include "utilities/memory/memory_resource_manager.h"
#include "utilities/string/string_helpers.h"

#include "processing_test_support.h"
#include "sensor_events_test_support.h"

#ifndef CONFIG_LOG_MODE_MINIMAL
    #include <zephyr/logging/log_backend.h>
    #include <zephyr/logging/log_core.h>
    #include <zephyr/logging/log_ctrl.h>
    #include <zephyr/logging/log_msg.h>
#endif

using eerie_leap::configuration::services::CborConfigurationService;
using eerie_leap::domain::sensor_domain::configuration::SensorsConfigurationManager;
using eerie_leap::domain::sensor_domain::event_bus::SensorEventsChannel;
using eerie_leap::domain::sensor_domain::event_bus::SensorEventType;
using eerie_leap::domain::sensor_domain::event_bus::SensorPayloadType;
using eerie_leap::domain::sensor_domain::isr_sensor_readers::ControlSensorReader;
using eerie_leap::domain::sensor_domain::isr_sensor_readers::IsrSensorReaderFactory;
using eerie_leap::domain::sensor_domain::models::IndicatorInputMode;
using eerie_leap::domain::sensor_domain::models::ReadingStatus;
using eerie_leap::domain::sensor_domain::models::Sensor;
using eerie_leap::domain::sensor_domain::models::SensorReading;
using eerie_leap::domain::sensor_domain::models::SensorType;
using eerie_leap::domain::sensor_domain::processors::IReadingProcessor;
using eerie_leap::domain::sensor_domain::runtime::SensorGeneration;
using eerie_leap::domain::sensor_domain::runtime::SensorPipelineBuilder;
using eerie_leap::domain::sensor_domain::runtime::SensorRuntime;
using eerie_leap::domain::sensor_domain::services::SensorEventsPublisher;
using eerie_leap::domain::sensor_domain::services::SensorsProcessingService;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;
using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::event_bus::EventData;
using eerie_leap::subsys::fs::services::FsService;
using eerie_leap::subsys::threading::ScopedMutex;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::subsys::time::TimeService;
using eerie_leap::utilities::memory::Mrm;
using eerie_leap::utilities::string::StringHelpers;

using processing_test_support::MakeFormattedFsService;
using processing_test_support::MakeTimeService;
using sensor_events_test_support::Bind;
using sensor_events_test_support::Recorder;
using sensor_events_test_support::RegisterSensorEvents;

namespace {

// A failure bound only; nothing waits on time to synchronise.
constexpr int TIMEOUT_MS = 1000;

constexpr int HELPER_PRIORITY = 5;

K_THREAD_STACK_DEFINE(publisher_stack, 8192);
k_thread publisher_thread;

void* SetUp() {
    RegisterSensorEvents();

    return nullptr;
}

} // namespace

ZTEST_SUITE(control_sensors, NULL, SetUp, NULL, NULL, NULL);

namespace {

// ======== The log ========

// Whether the log has backends to count with. The minimal log mode that ztest selects off native_sim
// prints to printk and has none, so there the counts are not asserted and the cases check only what
// the code does.
constexpr bool CAN_COUNT_LOGS = !IS_ENABLED(CONFIG_LOG_MODE_MINIMAL);

// Counts the warnings and errors of one logger, so a case can assert that a drop was logged. The
// counting backend is defined once for the binary, below; a case names the logger with Watch(),
// provokes, and reads Count().
class LogCounter {
private:
    k_spinlock lock_{};
    const char* source_name_ = nullptr;
    size_t count_ = 0;

    LogCounter() = default;

public:
    static LogCounter& Get() {
        static LogCounter counter;

        return counter;
    }

    // Counts messages of the logger @p source_name (its module name) at WRN or worse from now on.
    void Watch(const char* source_name) {
        K_SPINLOCK(&lock_) {
            source_name_ = source_name;
            count_ = 0;
        }
    }

    // Flushes the deferred log first, so every message of the case has been processed.
    size_t Count() {
#ifndef CONFIG_LOG_MODE_MINIMAL
        log_flush();
#endif

        size_t count = 0;

        K_SPINLOCK(&lock_) {
            count = count_;
        }

        return count;
    }

    // The backend's entry point. Any context: native_sim logs in immediate mode, which calls the
    // backend in the context of the logging caller, so no mutex.
    void Add(const char* source_name) {
        K_SPINLOCK(&lock_) {
            if(source_name_ != nullptr && source_name != nullptr && std::string_view(source_name) == source_name_)
                count_++;
        }
    }
};

// ======== Sensors ========

std::shared_ptr<Sensor> MakeControlIndicator(
    std::string_view id,
    uint32_t debounce_ms = 0,
    IndicatorInputMode input_mode = IndicatorInputMode::Level
) {

    auto sensor = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), id);
    sensor->configuration.type = SensorType::CONTROL_INDICATOR;
    sensor->configuration.debounce_ms = debounce_ms;
    sensor->configuration.input_mode = input_mode;

    return sensor;
}

std::shared_ptr<Sensor> MakeControlAnalog(std::string_view id, std::string_view expression = {}) {
    auto sensor = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), id);
    sensor->configuration.type = SensorType::CONTROL_ANALOG;
    sensor->configuration.expression = expression;

    return sensor;
}

// A VIRTUAL_ANALOG without a rate that reads one sensor, so it is evaluated after every commit of
// it, with the committed value.
std::shared_ptr<Sensor> MakeProbe(std::string_view id, std::string_view input_id) {
    auto sensor = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), id);
    sensor->configuration.type = SensorType::VIRTUAL_ANALOG;
    sensor->configuration.expression = input_id;

    return sensor;
}

// ======== Inputs ========

SensorEventsChannel::EventMessage MakeInput(std::string_view sensor_id, EventData value) {
    SensorEventsChannel::EventMessage event{ .source_id = 0xC0DE, .type = SensorEventType::InputRequested };
    event.payload.insert_or_assign(SensorPayloadType::SensorId, EventData(StringHelpers::GetHash(sensor_id)));
    event.payload.insert_or_assign(SensorPayloadType::Value, std::move(value));

    return event;
}

// What a control's outbound binding does: the value goes to the bus worker, which delivers it.
void PublishAsync(std::string_view sensor_id, EventData value) {
    SensorEventsChannel::GetInstance().PublishAsync(MakeInput(sensor_id, std::move(value)));
}

// Delivered on this thread before it returns, so a case that publishes a value nobody should take can
// tell it was not queued behind anything.
void Publish(std::string_view sensor_id, EventData value) {
    SensorEventsChannel::GetInstance().Publish(MakeInput(sensor_id, std::move(value)));
}

// For the cases that run on every board, where nothing is timed exactly.
template<typename Predicate>
bool WaitFor(Predicate predicate) {
    const int64_t deadline = k_uptime_get() + TIMEOUT_MS;

    while(!predicate()) {
        if(k_uptime_get() >= deadline)
            return false;

        k_msleep(5);
    }

    return true;
}

// ======== The processing service ========

// The last stage: records the value of every reading that reaches it, by sensor. For a control
// sensor that is its input after the expression and before the conditioner; a probe of it is
// evaluated once per commit, so its records are the commit history of the control sensor.
class ReadingRecorder : public IReadingProcessor {
private:
    struct Entry {
        uint32_t sensor_id_hash;
        float value;
    };

    mutable k_mutex lock_{};
    std::vector<Entry> entries_;

public:
    ReadingRecorder() {
        k_mutex_init(&lock_);
        entries_.reserve(128);
    }

    void Process(const SensorRuntime& runtime, SensorReading& reading) override {
        if(!reading.value.has_value())
            return;

        const ScopedMutex guard(lock_);
        entries_.push_back({ runtime.sensor->id_hash, reading.value.value() });
    }

    [[nodiscard]] std::vector<float> Of(std::string_view sensor_id) const {
        const uint32_t sensor_id_hash = StringHelpers::GetHash(sensor_id);

        const ScopedMutex guard(lock_);

        std::vector<float> values;
        for(const auto& entry : entries_) {
            if(entry.sensor_id_hash == sensor_id_hash)
                values.push_back(entry.value);
        }

        return values;
    }

    [[nodiscard]] size_t CountOf(std::string_view sensor_id) const {
        return Of(sensor_id).size();
    }

    [[nodiscard]] size_t Count() const {
        const ScopedMutex guard(lock_);

        return entries_.size();
    }

    void Clear() {
        const ScopedMutex guard(lock_);

        entries_.clear();
    }
};

// The processing service wired as SensorsController wires it without GPIO, CAN or polled sensors,
// over a stored sensors configuration, with the recorder last. Initialized, not started.
struct ControlRig {
    std::shared_ptr<FsService> fs_service;
    std::shared_ptr<TimeService> time_service;
    std::shared_ptr<SensorReadingsFrame> frame;
    std::shared_ptr<SensorsConfigurationManager> sensors_configuration_manager;
    std::shared_ptr<SensorsProcessingService> processing_service;
    std::shared_ptr<ReadingRecorder> recorder;
    std::unique_ptr<SensorEventsPublisher> publisher;

    [[nodiscard]] float Value(std::string_view id) const {
        return frame->TryGetReadingValue(id).value_or(NAN);
    }

    [[nodiscard]] bool WaitForValue(std::string_view id, float expected) const {
        return WaitFor([&] {
            return frame->TryGetReadingValue(id) == expected;
        });
    }

    [[nodiscard]] bool WaitForReadings(std::string_view id, size_t count) const {
        return WaitFor([&] {
            return recorder->CountOf(id) >= count;
        });
    }

    // As SensorsController::RestartProcessing() does after an apply.
    void Restart() const {
        zassert_true(processing_service->Stop());
        zassert_true(processing_service->Start());
    }
};

ControlRig MakeRig(const std::vector<std::shared_ptr<Sensor>>& sensors, bool is_publishing = false) {
    ControlRig rig;
    rig.fs_service = MakeFormattedFsService();
    rig.time_service = MakeTimeService();
    rig.frame = std::make_shared<SensorReadingsFrame>();

    rig.sensors_configuration_manager = std::make_shared<SensorsConfigurationManager>(
        std::make_unique<CborConfigurationService<CborSensorsConfig>>("sensors_config", rig.fs_service), nullptr, 0, 0
    );
    zassert_true(rig.sensors_configuration_manager->Update(sensors), "The sensors configuration is valid");

    rig.processing_service = std::make_shared<SensorsProcessingService>(
        rig.sensors_configuration_manager, rig.frame, rig.time_service,
        std::make_shared<IsrSensorReaderFactory>(rig.time_service, nullptr), nullptr
    );
    zassert_true(rig.processing_service->Initialize());

    rig.recorder = std::make_shared<ReadingRecorder>();
    zassert_true(rig.processing_service->RegisterReadingProcessor(rig.recorder));

    if(is_publishing) {
        rig.publisher = std::make_unique<SensorEventsPublisher>(rig.frame);
        zassert_true(rig.publisher->Initialize());
        zassert_true(rig.publisher->Start());
    }

    return rig;
}

// A press and its release, one after the other, through the bus worker.
void Press(const ControlRig& rig, const std::string& id) {
    const size_t before = rig.recorder->CountOf(id);

    PublishAsync(id, EventData(1.0F));
    PublishAsync(id, EventData(0.0F));

    zassert_true(rig.WaitForReadings(id, before + 2), "%s: the press and its release were processed", id.c_str());
}

// ======== A generation, and a reader on a work queue of its own ========

// A generation with the frame its expressions are bound to; nothing runs on it.
struct Built {
    std::shared_ptr<SensorReadingsFrame> frame;
    std::shared_ptr<SensorGeneration> generation;

    const SensorGeneration& operator*() const {
        return *generation;
    }
    const SensorGeneration* operator->() const {
        return generation.get();
    }
};

Built Build(const std::vector<std::shared_ptr<Sensor>>& sensors) {
    Built built;
    built.frame = std::make_shared<SensorReadingsFrame>();
    built.generation = SensorPipelineBuilder(nullptr, built.frame)
                           .Build(std::make_shared<const std::vector<std::shared_ptr<Sensor>>>(sensors));

    return built;
}

// A control reader whose callback holds the first reading it is given until the case releases it:
// the values published meanwhile wait in the reader's queue. On the heap, so a case that fails, and
// so never destroys it, leaves no subscription pointing into a dead stack frame.
class GatedReader {
private:
    mutable k_mutex lock_{};
    k_sem entered_{};
    k_sem gate_{};
    std::vector<float> received_;

    Built built_;
    std::shared_ptr<WorkQueueThread> work_queue_thread_;
    std::unique_ptr<ControlSensorReader> reader_;

    void Receive(float value) {
        size_t position = 0;

        {
            const ScopedMutex guard(lock_);

            received_.push_back(value);
            position = received_.size();
        }

        if(position == 1) {
            k_sem_give(&entered_);

            // Bounded, so a case that fails does not hold the work queue for good.
            k_sem_take(&gate_, K_SECONDS(5));
        }
    }

public:
    explicit GatedReader(const std::shared_ptr<Sensor>& sensor) {
        k_mutex_init(&lock_);
        k_sem_init(&entered_, 0, K_SEM_MAX_LIMIT);
        k_sem_init(&gate_, 0, K_SEM_MAX_LIMIT);

        built_ = Build({ sensor });

        work_queue_thread_ = std::make_shared<WorkQueueThread>("control_sensors_test", 8192, 6);
        zassert_true(work_queue_thread_->Initialize());

        reader_ = std::make_unique<ControlSensorReader>(
            MakeTimeService(), *built_->Find(sensor->id_hash),
            [this](const SensorRuntime&, SensorReading& reading) {
                Receive(reading.value.value_or(NAN));
            },
            work_queue_thread_
        );
    }

    ~GatedReader() {
        k_sem_give(&gate_);
        reader_.reset();
    }

    GatedReader(const GatedReader&) = delete;
    GatedReader& operator=(const GatedReader&) = delete;
    GatedReader(GatedReader&&) = delete;
    GatedReader& operator=(GatedReader&&) = delete;

    // Whether the callback holds the first reading.
    [[nodiscard]] bool WaitForFirstReading() {
        return k_sem_take(&entered_, K_MSEC(TIMEOUT_MS)) == 0;
    }

    void Release() {
        k_sem_give(&gate_);
    }

    [[nodiscard]] std::vector<float> Received() const {
        const ScopedMutex guard(lock_);

        return received_;
    }

    [[nodiscard]] bool WaitForReceived(size_t count) const {
        return WaitFor([&] {
            return Received().size() >= count;
        });
    }

    [[nodiscard]] uint32_t GetDroppedCount() const {
        return reader_->GetDroppedCount();
    }
};

// A value a control may publish, with what an indicator and an analog sensor read of it.
struct NumberCase {
    EventData value;
    float level;
    float number;
};

// ======== Concurrent publishing ========

// Shared by the case and the helper thread, so a failed assertion, which aborts the case without
// unwinding, never leaves the helper writing to a dead stack frame.
struct PublishRace {
    std::atomic<bool> is_stopping{ false };
    std::atomic<int> published{ 0 };
};

// A long way past what a case needs, so the helper ends by itself if the case does not stop it.
constexpr int MAX_PUBLISHES = 5000;

void PublishUntilStopped(void* race_holder, void*, void*) {
    const std::unique_ptr<std::shared_ptr<PublishRace>> holder(static_cast<std::shared_ptr<PublishRace>*>(race_holder));
    auto& race = **holder;

    for(int i = 0; i < MAX_PUBLISHES && !race.is_stopping.load(); i++) {
        Publish("ctl_race", EventData(i % 2 == 0 ? 1.0F : 0.0F));
        race.published++;

        k_msleep(1);
    }
}

} // namespace

#ifndef CONFIG_LOG_MODE_MINIMAL

namespace {

void ProcessLogMessage(const log_backend* const backend, log_msg_generic* message) {
    ARG_UNUSED(backend);

    log_msg* log_message = &message->log;
    if(log_msg_get_level(log_message) > LOG_LEVEL_WRN)
        return;

    const int16_t source_id = log_msg_get_source_id(log_message);
    if(source_id < 0)
        return;

    LogCounter::Get().Add(log_source_name_get(Z_LOG_LOCAL_DOMAIN_ID, static_cast<uint32_t>(source_id)));
}

void DropLogMessages(const log_backend* const backend, uint32_t count) {
    ARG_UNUSED(backend);
    ARG_UNUSED(count);
}

void PanicLog(const log_backend* const backend) {
    ARG_UNUSED(backend);
}

const log_backend_api LOG_COUNTER_API = {
    .process = ProcessLogMessage,
    .dropped = DropLogMessages,
    .panic = PanicLog,
};

} // namespace

// Autostarted at the default level, which passes the warnings it counts.
LOG_BACKEND_DEFINE(CONTROL_SENSORS_LOG_COUNTER, LOG_COUNTER_API, true);

#endif // !CONFIG_LOG_MODE_MINIMAL

// ======== The sensor type ========

ZTEST(control_sensors, test_only_an_enabled_control_indicator_gives_the_start_a_value) {
    const auto disabled = MakeControlIndicator("ctl_start_values_disabled");
    disabled->is_enabled = false;

    zassert_true(Build({ MakeControlIndicator("ctl_start_values_level") })->HasStartValues());
    zassert_true(
        Build({ MakeControlIndicator("ctl_start_values_toggle", 30, IndicatorInputMode::Toggle) })->HasStartValues()
    );
    zassert_false(Build({ MakeControlAnalog("ctl_start_values_analog") })->HasStartValues(), "No position yet");
    zassert_false(Build({ disabled })->HasStartValues(), "A disabled sensor has no source");
}

ZTEST(control_sensors, test_the_factory_builds_a_reader_for_a_control_sensor_only) {
    const auto gpio_indicator = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "ctl_factory_gpio");
    gpio_indicator->configuration.type = SensorType::PHYSICAL_INDICATOR;
    gpio_indicator->configuration.channel = 0;

    const auto generation = Build(
        { MakeControlIndicator("ctl_factory_indicator"), MakeControlAnalog("ctl_factory_analog"), gpio_indicator,
            MakeProbe("ctl_factory_probe", "ctl_factory_analog") }
    );

    auto work_queue_thread = std::make_shared<WorkQueueThread>("control_sensors_test", 8192, 6);
    zassert_true(work_queue_thread->Initialize());

    IsrSensorReaderFactory factory(MakeTimeService(), nullptr);
    const auto callback = [](const SensorRuntime&, SensorReading&) {};
    const auto create = [&](std::string_view id) {
        return factory.Create(*generation->Find(StringHelpers::GetHash(id)), work_queue_thread, callback);
    };

    zassert_not_null(create("ctl_factory_indicator").get());
    zassert_not_null(create("ctl_factory_analog").get());
    zassert_is_null(create("ctl_factory_gpio").get(), "The unit has no GPIO");
    zassert_is_null(create("ctl_factory_probe").get(), "A virtual sensor has no event-driven reader");

    // The reader itself refuses a sensor that no control feeds.
    bool is_refused = false;
    try {
        const ControlSensorReader reader(
            MakeTimeService(), *generation->Find(StringHelpers::GetHash("ctl_factory_gpio")), callback,
            work_queue_thread
        );
    } catch(const std::runtime_error&) {
        is_refused = true;
    }

    zassert_true(is_refused);
}

// ======== What a control publishes ========

ZTEST(control_sensors, test_an_input_drives_a_level_control_indicator_and_DataUpdated_follows) {
    const auto follow = MakeControlIndicator("ctl_follow");
    auto rig = MakeRig({ follow }, true);

    const auto deliveries = std::make_shared<Recorder>();
    const AnySubscription subscription = Bind(*follow, deliveries);
    zassert_not_null(subscription.get());

    zassert_true(rig.processing_service->Start());
    zassert_true(deliveries->WaitForDelivery(), "The start's 0 was published");
    zassert_equal(deliveries->At(0), 0.0F);

    PublishAsync("ctl_follow", EventData(1.0F));
    zassert_true(deliveries->WaitForDelivery(), "The 1 was published");
    zassert_equal(deliveries->At(1), 1.0F);
    zassert_equal(rig.Value("ctl_follow"), 1.0F);

    PublishAsync("ctl_follow", EventData(0.0F));
    zassert_true(deliveries->WaitForDelivery(), "The 0 was published");
    zassert_equal(deliveries->At(2), 0.0F);
    zassert_equal(rig.Value("ctl_follow"), 0.0F);

    zassert_true(rig.processing_service->Stop());
    rig.publisher->Stop();
}

ZTEST(control_sensors, test_another_id_a_text_and_NaN_change_nothing) {
    auto rig = MakeRig({ MakeControlIndicator("ctl_ignore_a"), MakeControlIndicator("ctl_ignore_b") });
    zassert_true(rig.processing_service->Start());

    zassert_equal(rig.recorder->CountOf("ctl_ignore_a"), 1, "The start's released input");
    zassert_equal(rig.recorder->CountOf("ctl_ignore_b"), 1);

    Publish("ctl_ignore_a", EventData(1.0F));
    zassert_true(rig.WaitForValue("ctl_ignore_a", 1.0F));
    zassert_equal(rig.Value("ctl_ignore_b"), 0.0F, "Another sensor's input does not reach it");
    zassert_equal(rig.recorder->CountOf("ctl_ignore_b"), 1);

    // An id no sensor has, a text, and a number that is none.
    Publish("ctl_ignore_nobody", EventData(1.0F));
    Publish("ctl_ignore_a", EventData(std::string("on")));
    Publish("ctl_ignore_a", EventData(std::numeric_limits<float>::quiet_NaN()));

    // Published on this thread, so a value that was taken is queued by now, and processing it takes
    // less than this.
    k_msleep(50);
    zassert_equal(rig.recorder->CountOf("ctl_ignore_a"), 2, "Neither the text nor the NaN was processed");
    zassert_equal(rig.recorder->CountOf("ctl_ignore_b"), 1);
    zassert_equal(rig.Value("ctl_ignore_a"), 1.0F);

    // The sensor still listens.
    Publish("ctl_ignore_a", EventData(0.0F));
    zassert_true(rig.WaitForValue("ctl_ignore_a", 0.0F));
    zassert_equal(rig.recorder->CountOf("ctl_ignore_a"), 3);

    zassert_true(rig.processing_service->Stop());
}

ZTEST(control_sensors, test_an_indicator_takes_a_number_as_a_level_and_an_analog_sensor_as_it_is) {
    auto rig = MakeRig({ MakeControlIndicator("ctl_number_level"), MakeControlAnalog("ctl_number_analog") });
    zassert_true(rig.processing_service->Start());

    const std::vector<NumberCase> cases{
        { EventData(true), 1.0F, 1.0F },
        { EventData(false), 0.0F, 0.0F },
        { EventData(5), 1.0F, 5.0F },
        { EventData(0), 0.0F, 0.0F },
        { EventData(-3), 1.0F, -3.0F },
        { EventData(uint32_t{ 7 }), 1.0F, 7.0F },
        { EventData(uint32_t{ 0 }), 0.0F, 0.0F },
        { EventData(0.5F), 1.0F, 0.5F },
        { EventData(0.0F), 0.0F, 0.0F },
        { EventData(-2.5F), 1.0F, -2.5F },
    };

    // The indicator's start is a released input; the analog sensor has none.
    std::vector<float> levels{ 0.0F };
    std::vector<float> numbers;

    for(size_t i = 0; i < cases.size(); i++) {
        Publish("ctl_number_level", cases[i].value);
        Publish("ctl_number_analog", cases[i].value);

        levels.push_back(cases[i].level);
        numbers.push_back(cases[i].number);

        // Each is processed before the next goes, as an analog sensor keeps only its newest value.
        zassert_true(rig.WaitForReadings("ctl_number_level", levels.size()), "case %zu", i);
        zassert_true(rig.WaitForReadings("ctl_number_analog", numbers.size()), "case %zu", i);
    }

    zassert_true(rig.recorder->Of("ctl_number_level") == levels);
    zassert_true(rig.recorder->Of("ctl_number_analog") == numbers);

    zassert_true(rig.WaitForValue("ctl_number_level", levels.back()));
    zassert_true(rig.WaitForValue("ctl_number_analog", numbers.back()));

    zassert_true(rig.processing_service->Stop());
}

// ======== Toggle ========

ZTEST(control_sensors, test_a_toggle_control_indicator_flips_at_each_press_the_first_after_a_start_included) {
    auto rig = MakeRig(
        { MakeControlIndicator("ctl_toggle", 0, IndicatorInputMode::Toggle),
            MakeProbe("ctl_toggle_probe", "ctl_toggle") }
    );
    zassert_true(rig.processing_service->Start());
    zassert_equal(rig.Value("ctl_toggle"), 0.0F, "A switch starts off");

    rig.recorder->Clear();

    for(const float expected : { 1.0F, 0.0F, 1.0F }) {
        Press(rig, "ctl_toggle");

        zassert_equal(rig.Value("ctl_toggle"), expected);
    }

    // The press flips the switch, its release commits it as it is. The probe is evaluated after each commit.
    const std::vector<float> commits{ 1.0F, 1.0F, 0.0F, 0.0F, 1.0F, 1.0F };
    zassert_true(rig.WaitForReadings("ctl_toggle_probe", commits.size()));
    zassert_true(rig.recorder->Of("ctl_toggle_probe") == commits);

    zassert_true(rig.processing_service->Stop());
}

ZTEST(control_sensors, test_presses_published_back_to_back_each_flip_a_toggle) {
    auto rig = MakeRig(
        { MakeControlIndicator("ctl_b2b", 0, IndicatorInputMode::Toggle), MakeProbe("ctl_b2b_probe", "ctl_b2b") }
    );
    zassert_true(rig.processing_service->Start());

    rig.recorder->Clear();

    // Three presses and their releases without waiting between them: they queue on the bus worker
    // and in the reader, and each is processed in order.
    for(int press = 0; press < 3; press++) {
        PublishAsync("ctl_b2b", EventData(1.0F));
        PublishAsync("ctl_b2b", EventData(0.0F));
    }

    // The probe is evaluated after each commit, so its sixth record is the last thing that happens.
    const std::vector<float> commits{ 1.0F, 1.0F, 0.0F, 0.0F, 1.0F, 1.0F };
    zassert_true(rig.WaitForReadings("ctl_b2b_probe", commits.size()));
    zassert_true(rig.recorder->Of("ctl_b2b_probe") == commits);
    zassert_equal(rig.Value("ctl_b2b"), 1.0F, "Three flips");

    zassert_true(rig.processing_service->Stop());
}

ZTEST(control_sensors, test_with_debounce_a_press_shorter_than_the_window_changes_nothing_and_a_hold_acts) {
    constexpr int DEBOUNCE_MS = 200;

    auto rig = MakeRig(
        { MakeControlIndicator("ctl_hold", DEBOUNCE_MS, IndicatorInputMode::Toggle),
            MakeProbe("ctl_hold_probe", "ctl_hold") }
    );
    zassert_true(rig.processing_service->Start());

    rig.recorder->Clear();

    // A tap: down and up well inside the window.
    PublishAsync("ctl_hold", EventData(1.0F));
    k_msleep(DEBOUNCE_MS / 5);
    PublishAsync("ctl_hold", EventData(0.0F));

    zassert_true(rig.WaitForReadings("ctl_hold", 2));
    k_msleep(2 * DEBOUNCE_MS);
    zassert_equal(rig.Value("ctl_hold"), 0.0F, "A tap does nothing");

    // A hold: down for longer than the window.
    PublishAsync("ctl_hold", EventData(1.0F));
    zassert_true(rig.WaitForValue("ctl_hold", 1.0F), "A hold acts once it has held the window");

    PublishAsync("ctl_hold", EventData(0.0F));
    zassert_true(rig.WaitForReadings("ctl_hold", 4));
    k_msleep(2 * DEBOUNCE_MS);
    zassert_equal(rig.Value("ctl_hold"), 1.0F, "The release leaves the switch on");

    // The tap committed its release as it was, the hold flipped the switch once and its release left it.
    const std::vector<float> commits{ 0.0F, 1.0F, 1.0F };
    zassert_true(rig.recorder->Of("ctl_hold_probe") == commits);

    // Another hold turns it off.
    PublishAsync("ctl_hold", EventData(1.0F));
    zassert_true(rig.WaitForValue("ctl_hold", 0.0F));
    PublishAsync("ctl_hold", EventData(0.0F));

    zassert_true(rig.processing_service->Stop());
}

// ======== The start, a pause and a restart ========

ZTEST(control_sensors, test_at_a_start_a_level_control_indicator_reads_0_and_a_control_analog_has_no_value) {
    const auto disabled = MakeControlIndicator("ctl_start_disabled");
    disabled->is_enabled = false;

    auto rig = MakeRig(
        { MakeControlIndicator("ctl_start_level"), MakeControlAnalog("ctl_start_analog"), disabled,
            MakeProbe("ctl_start_probe", "ctl_start_level") }
    );
    zassert_true(rig.processing_service->Start());

    const auto level = rig.frame->TryGetReading("ctl_start_level");
    zassert_true(level.has_value(), "Start() returns with the released input committed");
    zassert_equal(level->value.value(), 0.0F);
    zassert_equal(level->raw_value.value(), 0.0F);
    zassert_equal(level->status, ReadingStatus::PROCESSED);
    zassert_equal(rig.Value("ctl_start_probe"), 0.0F, "Its dependents were evaluated");

    zassert_false(
        rig.frame->TryGetReading("ctl_start_analog").has_value(), "No position is a slider's before it moves"
    );
    zassert_false(rig.frame->TryGetReading("ctl_start_disabled").has_value(), "A disabled sensor reads nothing");

    zassert_true(rig.processing_service->Stop());
}

ZTEST(control_sensors, test_a_restart_gives_a_level_control_indicator_0_again) {
    auto rig = MakeRig({ MakeControlIndicator("ctl_restart_level") });
    zassert_true(rig.processing_service->Start());

    Publish("ctl_restart_level", EventData(1.0F));
    zassert_true(rig.WaitForValue("ctl_restart_level", 1.0F));

    rig.Restart();
    zassert_equal(rig.Value("ctl_restart_level"), 0.0F, "Released again when Start() returns");

    Publish("ctl_restart_level", EventData(1.0F));
    zassert_true(rig.WaitForValue("ctl_restart_level", 1.0F), "The new reader listens");

    zassert_true(rig.processing_service->Stop());
}

ZTEST(control_sensors, test_a_restart_keeps_the_switch_of_a_toggle_and_its_next_press_flips_it) {
    auto rig = MakeRig(
        { MakeControlIndicator("ctl_restart_toggle", 0, IndicatorInputMode::Toggle),
            MakeProbe("ctl_restart_probe", "ctl_restart_toggle") }
    );
    zassert_true(rig.processing_service->Start());

    Press(rig, "ctl_restart_toggle");
    zassert_equal(rig.Value("ctl_restart_toggle"), 1.0F);

    rig.Restart();
    zassert_equal(rig.Value("ctl_restart_toggle"), 1.0F, "The switch is kept");
    zassert_equal(rig.Value("ctl_restart_probe"), 1.0F);

    // The released input of the start was its first input, so this press is a flip.
    Press(rig, "ctl_restart_toggle");
    zassert_equal(rig.Value("ctl_restart_toggle"), 0.0F);

    zassert_true(rig.processing_service->Stop());
}

ZTEST(control_sensors, test_a_value_survives_Pause_and_Resume_and_an_input_during_the_pause_is_ignored) {
    auto rig = MakeRig(
        { MakeControlIndicator("ctl_pause_level"),
            MakeControlIndicator("ctl_pause_toggle", 0, IndicatorInputMode::Toggle),
            MakeControlAnalog("ctl_pause_analog") }
    );
    zassert_true(rig.processing_service->Start());

    Publish("ctl_pause_level", EventData(1.0F));
    Publish("ctl_pause_analog", EventData(42.0F));
    zassert_true(rig.WaitForValue("ctl_pause_level", 1.0F));
    zassert_true(rig.WaitForValue("ctl_pause_analog", 42.0F));
    Press(rig, "ctl_pause_toggle");
    zassert_equal(rig.Value("ctl_pause_toggle"), 1.0F);

    zassert_true(rig.processing_service->Pause());

    // No reader listens now. Published on this thread, so a reader that did would have taken it.
    Publish("ctl_pause_level", EventData(0.0F));
    Publish("ctl_pause_analog", EventData(7.0F));

    zassert_true(rig.processing_service->Resume());
    k_msleep(50);

    zassert_equal(rig.Value("ctl_pause_level"), 1.0F, "Resume() commits no released input, nor the paused input");
    zassert_equal(rig.Value("ctl_pause_analog"), 42.0F);
    zassert_equal(rig.Value("ctl_pause_toggle"), 1.0F, "A switch keeps its value");

    // The readers built again listen.
    Publish("ctl_pause_level", EventData(0.0F));
    zassert_true(rig.WaitForValue("ctl_pause_level", 0.0F));
    Press(rig, "ctl_pause_toggle");
    zassert_equal(rig.Value("ctl_pause_toggle"), 0.0F, "Its next press flips it");

    zassert_true(rig.processing_service->Stop());
}

// ======== A control analog ========

ZTEST(control_sensors, test_a_control_analog_has_no_value_before_its_first_input_and_its_expression_sees_x) {
    auto rig = MakeRig({ MakeControlAnalog("ctl_raw"), MakeControlAnalog("ctl_scaled", "x * 2 + 1") });
    zassert_true(rig.processing_service->Start());

    zassert_false(rig.frame->TryGetReading("ctl_raw").has_value());
    zassert_false(rig.frame->TryGetReadingValue("ctl_raw").has_value());
    zassert_false(rig.frame->TryGetReading("ctl_scaled").has_value());

    Publish("ctl_raw", EventData(10.0F));
    Publish("ctl_scaled", EventData(5.0F));

    zassert_true(rig.WaitForValue("ctl_raw", 10.0F));
    zassert_true(rig.WaitForValue("ctl_scaled", 11.0F), "x * 2 + 1 of 5");

    const auto scaled = rig.frame->TryGetReading("ctl_scaled");
    zassert_equal(scaled->raw_value.value(), 5.0F, "raw_value keeps what the control published");

    zassert_true(rig.processing_service->Stop());
}

ZTEST(control_sensors, test_a_control_analog_takes_values_published_back_to_back_as_the_newest) {
    auto rig = MakeRig({ MakeControlAnalog("ctl_drag") });
    zassert_true(rig.processing_service->Start());

    // A drag: one position per tick, faster than the queues may drain.
    for(const float position : { 10.0F, 20.0F, 30.0F, 40.0F, 50.0F })
        PublishAsync("ctl_drag", EventData(position));

    zassert_true(rig.WaitForValue("ctl_drag", 50.0F), "The newest is the one that stays");

    // Whichever of the others were taken before it came, they came in order.
    const auto values = rig.recorder->Of("ctl_drag");
    zassert_true(std::ranges::is_sorted(values));
    zassert_equal(values.back(), 50.0F);

    zassert_true(rig.processing_service->Stop());
}

// ======== The reader's queue ========

ZTEST(control_sensors, test_an_indicator_keeps_every_level_in_order) {
    const auto gated = std::make_unique<GatedReader>(MakeControlIndicator("ctl_gated_levels"));

    Publish("ctl_gated_levels", EventData(1.0F));
    zassert_true(gated->WaitForFirstReading(), "The callback holds the first level");

    // A press, its release and another press, while the work queue is busy.
    for(const float level : { 0.0F, 1.0F, 0.0F })
        Publish("ctl_gated_levels", EventData(level));

    gated->Release();
    zassert_true(gated->WaitForReceived(4));

    const std::vector<float> expected{ 1.0F, 0.0F, 1.0F, 0.0F };
    zassert_true(gated->Received() == expected);
    zassert_equal(gated->GetDroppedCount(), 0);
}

ZTEST(control_sensors, test_an_analog_sensor_keeps_only_its_newest_value) {
    const auto gated = std::make_unique<GatedReader>(MakeControlAnalog("ctl_gated_newest"));

    Publish("ctl_gated_newest", EventData(10.0F));
    zassert_true(gated->WaitForFirstReading(), "The callback holds the first value");

    for(const float position : { 20.0F, 30.0F, 40.0F })
        Publish("ctl_gated_newest", EventData(position));

    gated->Release();
    zassert_true(gated->WaitForReceived(2));
    k_msleep(50);

    const std::vector<float> expected{ 10.0F, 40.0F };
    zassert_true(gated->Received() == expected, "The 20 and the 30 were replaced by the 40");
    zassert_equal(gated->GetDroppedCount(), 0, "Replacing is no drop");
}

ZTEST(control_sensors, test_a_full_indicator_queue_drops_a_level_and_logs_it) {
    const auto gated = std::make_unique<GatedReader>(MakeControlIndicator("ctl_gated_full"));
    constexpr size_t CAPACITY = ControlSensorReader::QUEUE_CAPACITY;

    Publish("ctl_gated_full", EventData(1.0F));
    zassert_true(gated->WaitForFirstReading(), "The callback holds the first level");

    // The queue fills with alternating levels...
    for(size_t i = 0; i < CAPACITY; i++)
        Publish("ctl_gated_full", EventData(i % 2 == 0 ? 0.0F : 1.0F));
    zassert_equal(gated->GetDroppedCount(), 0, "Full, and nothing lost yet");

    // ...and the levels that find it full are dropped.
    LogCounter::Get().Watch("control_sensor_reader_logger");
    for(size_t i = 0; i < 3; i++)
        Publish("ctl_gated_full", EventData(i % 2 == 0 ? 1.0F : 0.0F));

    zassert_equal(gated->GetDroppedCount(), 3);
    if(CAN_COUNT_LOGS)
        zassert_equal(LogCounter::Get().Count(), 3, "Each drop is logged");

    gated->Release();
    zassert_true(gated->WaitForReceived(1 + CAPACITY));
    k_msleep(50);

    // The first level, then the sixteen that fitted, in order.
    const auto received = gated->Received();
    zassert_equal(received.size(), 1 + CAPACITY, "The dropped levels never came");
    zassert_equal(received[0], 1.0F);
    for(size_t i = 0; i < CAPACITY; i++)
        zassert_equal(received[1 + i], i % 2 == 0 ? 0.0F : 1.0F, "queued level %zu", i);
}

// ======== Stopping under a publisher ========

ZTEST(control_sensors, test_processing_stops_while_another_thread_keeps_publishing_input) {
    auto rig = MakeRig({ MakeControlIndicator("ctl_race") });
    zassert_true(rig.processing_service->Start());

    const auto race = std::make_shared<PublishRace>();
    k_thread_create(
        &publisher_thread, publisher_stack, K_THREAD_STACK_SIZEOF(publisher_stack), PublishUntilStopped,
        new std::shared_ptr<PublishRace>(race), nullptr, nullptr, HELPER_PRIORITY, 0, K_NO_WAIT
    );

    zassert_true(
        WaitFor([&] {
            return rig.recorder->CountOf("ctl_race") > 5;
        }),
        "The sensor reads what is published"
    );

    // The readers go and come back under the publisher: a handler in flight is waited for, and one
    // that was already taken by the channel is a no-op.
    for(int i = 0; i < 8; i++) {
        zassert_true(rig.processing_service->Pause());
        k_msleep(3);
        zassert_true(rig.processing_service->Resume());
        k_msleep(3);
    }

    zassert_true(rig.processing_service->Stop());

    const size_t processed = rig.recorder->Count();
    const int published = race->published.load();

    k_msleep(100);

    const size_t processed_later = rig.recorder->Count();
    const int published_later = race->published.load();

    race->is_stopping.store(true);
    zassert_ok(k_thread_join(&publisher_thread, K_SECONDS(5)));

    zassert_true(published_later > published, "The publisher went on after Stop()");
    zassert_equal(processed_later, processed, "Nothing was processed after Stop()");
}
