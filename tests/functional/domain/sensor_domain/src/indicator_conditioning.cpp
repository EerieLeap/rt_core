// Indicator input conditioning (docs/sensors.md, "Indicator conditioning"): which sensors get a
// conditioner, how a switch is carried into the next generation, and a pipeline driven by hand on
// every board. On native_sim, the debounce and the switch through the processing service, with
// edges from the emulated GPIO controller, frames from the loopback CAN controller and samples of
// a polled source, on simulated time: code takes no time there, an edge or a frame reaches the
// pipeline at the tick it was set or sent, and a settle lands on its deadline tick, so commit
// times are exact.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <eerie_memory.hpp>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "domain/sensor_domain/models/indicator_input_mode.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/sensor_domain/processors/expression_processor.h"
#include "domain/sensor_domain/processors/i_reading_processor.h"
#include "domain/sensor_domain/processors/indicator_conditioner.h"
#include "domain/sensor_domain/processors/reading_pipeline.hpp"
#include "domain/sensor_domain/runtime/sensor_pipeline_builder.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "subsys/gpio/i_gpio.h"
#include "subsys/threading/scoped_mutex.h"
#include "utilities/memory/memory_resource_manager.h"
#include "utilities/string/string_helpers.h"

#include "processing_test_support.h"

#ifdef CONFIG_BOARD_NATIVE_SIM
    #include <zephyr/device.h>
    #include <zephyr/devicetree.h>
    #include <zephyr/drivers/can.h>
    #include <zephyr/drivers/gpio.h>
    #include <zephyr/drivers/gpio/gpio_emul.h>
    #include <zephyr/sys/atomic.h>

    #include "configuration/cbor/cbor_canbus_config/cbor_canbus_config.h"
    #include "domain/canbus_domain/configuration/canbus_configuration_manager.h"
    #include "domain/canbus_domain/models/can_channel_configuration.h"
    #include "domain/canbus_domain/models/can_message_configuration.h"
    #include "domain/canbus_domain/models/canbus_configuration.h"
    #include "domain/canbus_domain/services/canbus_service.h"
    #include "domain/sensor_domain/models/sources/canbus_source.h"
    #include "subsys/canbus/can_id.h"
    #include "subsys/canbus/canbus_proxy.hpp"
    #include "subsys/gpio/gpio.h"
#endif

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::utilities::string;
using namespace eerie_leap::subsys::threading;
using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::sensor_domain::processors;
using namespace eerie_leap::domain::sensor_domain::runtime;
using namespace eerie_leap::domain::sensor_domain::utilities;

using namespace processing_test_support;

ZTEST_SUITE(indicator_conditioning, NULL, NULL, NULL, NULL, NULL);

namespace {

std::shared_ptr<Sensor> MakeIndicator(
    std::string_view id,
    int channel,
    uint32_t debounce_ms = 0,
    IndicatorInputMode input_mode = IndicatorInputMode::Level,
    std::optional<int> sampling_rate_ms = std::nullopt
) {

    auto sensor = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), id);
    sensor->configuration.type = SensorType::PHYSICAL_INDICATOR;
    sensor->configuration.channel = channel;
    sensor->configuration.sampling_rate_ms = sampling_rate_ms;
    sensor->configuration.debounce_ms = debounce_ms;
    sensor->configuration.input_mode = input_mode;

    return sensor;
}

// Evaluated whenever an input commits.
std::shared_ptr<Sensor> MakeDerivedIndicator(
    std::string_view id,
    std::string_view expression,
    uint32_t debounce_ms = 0,
    IndicatorInputMode input_mode = IndicatorInputMode::Level
) {

    auto sensor = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), id);
    sensor->configuration.type = SensorType::VIRTUAL_INDICATOR;
    sensor->configuration.expression = expression;
    sensor->configuration.debounce_ms = debounce_ms;
    sensor->configuration.input_mode = input_mode;

    return sensor;
}

// A VIRTUAL_ANALOG without a rate that reads one indicator, so it is evaluated after every commit
// of it, with the committed value.
std::shared_ptr<Sensor> MakeProbe(std::string_view id, std::string_view indicator_id) {
    auto sensor = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), id);
    sensor->configuration.type = SensorType::VIRTUAL_ANALOG;
    sensor->configuration.expression = indicator_id;

    return sensor;
}

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

Built Build(
    const std::vector<std::shared_ptr<Sensor>>& sensors,
    std::span<const IndicatorSwitchState> switch_states = {}
) {

    Built built;
    built.frame = std::make_shared<SensorReadingsFrame>();
    built.generation = SensorPipelineBuilder(nullptr, built.frame)
                           .Build(std::make_shared<const std::vector<std::shared_ptr<Sensor>>>(sensors), switch_states);

    return built;
}

const SensorRuntime& RuntimeOf(const SensorGeneration& generation, std::string_view id) {
    const auto* runtime = generation.Find(StringHelpers::GetHash(id));
    zassert_not_null(runtime, "%s is not in the generation", std::string(id).c_str());

    return *runtime;
}

const IndicatorConditioner* ConditionerOf(const SensorGeneration& generation, std::string_view id) {
    return RuntimeOf(generation, id).conditioner.get();
}

IndicatorSwitchState SwitchState(std::string_view id, SensorType type, uint32_t debounce_ms, bool is_on) {
    return { .sensor_id_hash = StringHelpers::GetHash(id),
        .type = type,
        .debounce_ms = debounce_ms,
        .input_mode = IndicatorInputMode::Toggle,
        .is_on = is_on };
}

// A reading as an edge source hands it to the pipeline.
SensorReading SourceReading(const SensorRuntime& runtime, float value) {
    SensorReading reading(runtime.sensor.get());
    reading.source = ReadingSource::ISR;
    reading.status = ReadingStatus::RAW;
    reading.value = value;
    reading.raw_value = value;

    return reading;
}

// A pipeline driven by hand, without a work queue: Process() runs on the test thread.
struct HandPipeline {
    std::shared_ptr<SensorReadingsFrame> frame;
    std::shared_ptr<SensorGeneration> generation;
    std::shared_ptr<ReadingPipeline> pipeline;

    void Feed(std::string_view id, float value) {
        auto reading = SourceReading(RuntimeOf(*generation, id), value);
        pipeline->Process(RuntimeOf(*generation, id), reading);
    }

    [[nodiscard]] float Value(std::string_view id) const {
        return frame->TryGetReadingValue(id).value_or(NAN);
    }
};

HandPipeline MakeHandPipeline(const std::vector<std::shared_ptr<Sensor>>& sensors) {
    HandPipeline hand;
    hand.frame = std::make_shared<SensorReadingsFrame>();
    hand.generation = SensorPipelineBuilder(nullptr, hand.frame)
                          .Build(std::make_shared<const std::vector<std::shared_ptr<Sensor>>>(sensors));

    const auto processors = std::make_shared<std::vector<std::shared_ptr<IReadingProcessor>>>();
    processors->push_back(std::make_shared<ExpressionProcessor>(hand.frame));
    hand.pipeline = std::make_shared<ReadingPipeline>(hand.frame, processors, MakeTimeService());

    return hand;
}

// Hands edges to the readers' handlers on the caller's thread, and throws from every registration
// while refusing, past the ISR reader factory's catch, as a source that fails to start.
class HandGpio : public IGpio {
private:
    struct Registration {
        int channel;
        int handler_id;
        eerie_leap::subsys::gpio::GpioChannelHandler handler;
    };

    std::vector<Registration> registrations_;
    int next_handler_id_ = 1;

public:
    bool is_refusing = false;

    int Initialize() override {
        return 0;
    }
    bool ReadChannel(int channel) override {
        ARG_UNUSED(channel);
        return false;
    }
    int GetChannelCount() override {
        return 4;
    }

    int RegisterChannelChangedHandler(
        int channel,
        eerie_leap::subsys::gpio::GpioEdge edge,
        eerie_leap::subsys::gpio::GpioChannelHandler handler
    ) override {

        ARG_UNUSED(edge);

        if(is_refusing)
            throw std::logic_error("GPIO channel refused");

        registrations_.push_back({ channel, next_handler_id_, std::move(handler) });

        return next_handler_id_++;
    }

    bool RemoveChannelChangedHandler(int channel, int handler_id) override {
        ARG_UNUSED(channel);

        std::erase_if(registrations_, [handler_id](const Registration& registration) {
            return registration.handler_id == handler_id;
        });

        return true;
    }

    void SetInput(int channel, bool level) {
        for(const auto& registration : registrations_) {
            if(registration.channel == channel)
                registration.handler(channel, level);
        }
    }
};

// For the cases that run on every board, where nothing is timed exactly.
template<typename Predicate>
bool WaitFor(Predicate predicate) {
    const int64_t deadline = k_uptime_get() + 1000;

    while(!predicate()) {
        if(k_uptime_get() >= deadline)
            return false;

        k_msleep(5);
    }

    return true;
}

} // namespace

ZTEST(indicator_conditioning, test_only_an_indicator_with_conditioning_gets_a_conditioner) {
    const auto analog = MakeAnalogSensor("cond_analog", 0, 100);
    // The validator refuses this; the builder still gives an analog sensor nothing.
    const auto debounced_analog = MakeAnalogSensor("cond_debounced_analog", 1, 100);
    debounced_analog->configuration.debounce_ms = 30;

    const auto plain = MakeIndicator("cond_plain", 0);
    const auto debounced = MakeIndicator("cond_debounced", 1, 30);
    const auto toggle = MakeDerivedIndicator("cond_toggle", "cond_plain", 0, IndicatorInputMode::Toggle);
    const auto disabled = MakeIndicator("cond_disabled", 2, 30, IndicatorInputMode::Toggle);
    disabled->is_enabled = false;

    const auto generation = Build({ analog, debounced_analog, plain, debounced, toggle, disabled });

    zassert_is_null(ConditionerOf(*generation, "cond_analog"));
    zassert_is_null(ConditionerOf(*generation, "cond_debounced_analog"));
    zassert_is_null(ConditionerOf(*generation, "cond_plain"), "The defaults build nothing");
    zassert_is_null(ConditionerOf(*generation, "cond_disabled"), "A disabled sensor gets nothing executable");

    const auto* debounced_conditioner = ConditionerOf(*generation, "cond_debounced");
    zassert_not_null(debounced_conditioner);
    zassert_false(debounced_conditioner->IsSwitch());

    const auto* toggle_conditioner = ConditionerOf(*generation, "cond_toggle");
    zassert_not_null(toggle_conditioner, "Toggle without a debounce is conditioning too");
    zassert_true(toggle_conditioner->IsSwitch());
    zassert_false(toggle_conditioner->IsSwitchOn(), "A switch without a state to take over starts off");

    zassert_true(generation->HasStartValues());
    zassert_false(Build({ plain, debounced })->HasStartValues());
}

ZTEST(indicator_conditioning, test_a_switch_takes_over_its_state_only_when_id_type_and_conditioning_are_unchanged) {
    const std::vector<IndicatorSwitchState> switch_states{
        SwitchState("take_same", SensorType::PHYSICAL_INDICATOR, 30, true),
        SwitchState("take_debounce", SensorType::PHYSICAL_INDICATOR, 40, true),
        SwitchState("take_type", SensorType::PHYSICAL_INDICATOR, 30, true),
        SwitchState("take_level", SensorType::PHYSICAL_INDICATOR, 30, true),
        SwitchState("take_off", SensorType::PHYSICAL_INDICATOR, 30, false),
    };

    const std::vector<std::shared_ptr<Sensor>> sensors{
        MakeIndicator("take_same", 0, 30, IndicatorInputMode::Toggle),
        MakeIndicator("take_debounce", 1, 30, IndicatorInputMode::Toggle),
        MakeDerivedIndicator("take_type", "take_same", 30, IndicatorInputMode::Toggle),
        MakeIndicator("take_level", 2, 30, IndicatorInputMode::Level),
        MakeIndicator("take_off", 3, 30, IndicatorInputMode::Toggle),
        MakeIndicator("take_unknown", 4, 30, IndicatorInputMode::Toggle),
    };

    const auto generation = Build(sensors, switch_states);

    zassert_true(ConditionerOf(*generation, "take_same")->IsSwitchOn(), "Unchanged: the switch is taken over");
    zassert_false(ConditionerOf(*generation, "take_debounce")->IsSwitchOn(), "Another debounce starts off");
    zassert_false(ConditionerOf(*generation, "take_type")->IsSwitchOn(), "Another type starts off");
    zassert_false(ConditionerOf(*generation, "take_level")->IsSwitch(), "A sensor now in Level mode is no switch");
    zassert_false(ConditionerOf(*generation, "take_off")->IsSwitchOn());
    zassert_false(ConditionerOf(*generation, "take_unknown")->IsSwitchOn(), "A new sensor starts off");

    zassert_false(ConditionerOf(*Build(sensors), "take_same")->IsSwitchOn(), "Without states every switch starts off");
}

ZTEST(indicator_conditioning, test_GetSwitchStates_lists_every_switch_with_its_conditioning) {
    const std::vector<IndicatorSwitchState> switch_states{
        SwitchState("list_on", SensorType::VIRTUAL_INDICATOR, 0, true),
    };

    const auto generation = Build(
        {
            MakeIndicator("list_plain", 0),
            MakeDerivedIndicator("list_on", "list_plain", 0, IndicatorInputMode::Toggle),
            MakeIndicator("list_level", 1, 30),
            MakeIndicator("list_off", 2, 20, IndicatorInputMode::Toggle),
        },
        switch_states
    );

    const auto states = generation->GetSwitchStates();
    zassert_equal(states.size(), 2, "%zu states", states.size());

    zassert_equal(states[0].sensor_id_hash, StringHelpers::GetHash("list_on"));
    zassert_equal(states[0].type, SensorType::VIRTUAL_INDICATOR);
    zassert_equal(states[0].debounce_ms, 0);
    zassert_equal(states[0].input_mode, IndicatorInputMode::Toggle);
    zassert_true(states[0].is_on);

    zassert_equal(states[1].sensor_id_hash, StringHelpers::GetHash("list_off"));
    zassert_equal(states[1].type, SensorType::PHYSICAL_INDICATOR);
    zassert_equal(states[1].debounce_ms, 20);
    zassert_equal(states[1].input_mode, IndicatorInputMode::Toggle);
    zassert_false(states[1].is_on);
}

// Without a debounce a Toggle input counts at once: each 0 -> 1 flips the value, a 1 -> 0 commits
// it unchanged, and the first value sets the input without a flip. The dependent sees the switch,
// raw_value the input.
ZTEST(indicator_conditioning, test_a_toggle_driven_by_hand_flips_on_each_press) {
    auto hand = MakeHandPipeline(
        {
            MakeIndicator("hand_toggle", 0, 0, IndicatorInputMode::Toggle),
            MakeProbe("hand_probe", "hand_toggle"),
        }
    );

    hand.Feed("hand_toggle", 1.0F);
    zassert_equal(hand.Value("hand_toggle"), 0.0F, "A button held at the first reading is no press");
    zassert_equal(hand.frame->TryGetReading("hand_toggle")->raw_value.value(), 1.0F);

    const float expected[][2] = { { 0.0F, 0.0F }, { 1.0F, 1.0F }, { 0.0F, 1.0F }, { 1.0F, 0.0F }, { 0.0F, 0.0F },
        { 1.0F, 1.0F } };
    for(const auto& [input, value] : expected) {
        hand.Feed("hand_toggle", input);

        zassert_equal(hand.Value("hand_toggle"), value, "input %.0f", static_cast<double>(input));
        zassert_equal(hand.Value("hand_probe"), value);
        zassert_equal(hand.frame->TryGetReading("hand_toggle")->raw_value.value(), input);
    }
}

// Without a queue nothing settles on a timer: a reading of the new value after the window commits
// it, and one back at the old value cancels it. A long window keeps the board's clock out of it.
ZTEST(indicator_conditioning, test_without_a_work_queue_a_held_change_settles_on_a_later_reading) {
    constexpr int DEBOUNCE_MS = 500;

    auto hand = MakeHandPipeline(
        {
            MakeIndicator("hand_level", 0, DEBOUNCE_MS),
            MakeProbe("hand_level_probe", "hand_level"),
        }
    );

    hand.Feed("hand_level", 0.0F);
    zassert_equal(hand.Value("hand_level"), 0.0F);

    hand.Feed("hand_level", 1.0F);
    zassert_equal(hand.Value("hand_level"), 0.0F, "Held");
    zassert_equal(
        hand.frame->TryGetReading("hand_level")->raw_value.value(), 0.0F, "A held reading is not committed at all"
    );

    hand.Feed("hand_level", 0.0F);
    k_msleep(DEBOUNCE_MS + 100);
    hand.Feed("hand_level", 1.0F);
    zassert_equal(hand.Value("hand_level"), 0.0F, "The return to 0 cancelled the window; this one starts a new one");

    k_msleep(DEBOUNCE_MS + 100);
    zassert_equal(hand.Value("hand_level"), 0.0F, "No timer without a queue");

    hand.Feed("hand_level", 1.0F);
    zassert_equal(hand.Value("hand_level"), 1.0F, "Confirmed after the window");
    zassert_equal(hand.Value("hand_level_probe"), 1.0F, "The dependent follows the commit");
}

// The switches come from the last generation that ran: a configuration that fails to start, here
// with another debounce that would start the switch off, takes nothing from them.
ZTEST(indicator_conditioning, test_a_failed_start_keeps_the_switches_of_the_last_generation_that_ran) {
    const auto gpio = std::make_shared<HandGpio>();
    const std::vector<std::shared_ptr<Sensor>> sensors{
        MakeIndicator("failed_start", 0, 0, IndicatorInputMode::Toggle)
    };

    auto rig = MakeProcessingRig(MakeFormattedFsService(), nullptr, nullptr, sensors, 0, gpio);
    zassert_true(rig.processing_service->Start());

    // The level at start goes first, or the press would coalesce with it and be taken as that level.
    zassert_true(
        WaitFor([&rig] {
            const auto reading = rig.frame->TryGetReading("failed_start");
            return reading.has_value() && reading->raw_value.has_value();
        }),
        "Seeded"
    );

    gpio->SetInput(0, true);
    zassert_true(
        WaitFor([&rig] {
            return rig.frame->TryGetReadingValue("failed_start") == 1.0F;
        }),
        "Switched on"
    );
    gpio->SetInput(0, false);

    zassert_true(
        rig.sensors_configuration_manager->Update({ MakeIndicator("failed_start", 0, 30, IndicatorInputMode::Toggle) })
    );
    zassert_true(rig.processing_service->Stop());

    gpio->is_refusing = true;
    bool is_thrown = false;
    try {
        rig.processing_service->Start();
    } catch(const std::logic_error&) {
        is_thrown = true;
    }
    gpio->is_refusing = false;

    zassert_true(is_thrown, "The start failed");
    zassert_true(rig.processing_service->IsStopped());

    zassert_true(rig.sensors_configuration_manager->Update(sensors));
    zassert_true(rig.processing_service->Start());
    zassert_equal(rig.frame->TryGetReadingValue("failed_start").value_or(NAN), 1.0F, "Still on when Start() returns");

    zassert_true(rig.processing_service->Stop());
}

#ifdef CONFIG_BOARD_NATIVE_SIM

namespace {

// The emulated controller of native_sim; led0 holds its pin 0.
const device* const GPIO_PORT = DEVICE_DT_GET(DT_NODELABEL(gpio0));
constexpr gpio_pin_t FIRST_PIN = 4;
constexpr int K_CHANNEL_COUNT = 28; // Pins 4 to 31.

// One channel per sensor and case: a case that fails ends without its destructors, so its readers
// stay attached to the channels it drove.
// NOLINTNEXTLINE(cppcoreguidelines-use-enum-class): plain channel numbers, used as indices and arguments throughout
enum Channel : int {
    kChAtOnce = 0,
    kChToggleAtOnce,
    kChShortEdge,
    kChHeldEdge,
    kChPolled,
    kChBounce,
    kChFirstValue,
    kChToggle,
    kChHeldAtStart,
    kChError,
    kChFollowed,
    kChContact,
    kChStop,
    kChSwitchAtStart,
    kChLevelAtStart,
    kChKeepEdge,
    kChKeepPolled,
    kChReset,
    kChResetInput,
    kChReboot,
    kChPause,
    kChannelsUsed,
};

static_assert(kChannelsUsed <= K_CHANNEL_COUNT);

// One Gpio for the suite: it never removes its callbacks from the driver.
std::shared_ptr<eerie_leap::subsys::gpio::Gpio> TestGpio() {
    static const auto GPIO = [] {
        std::vector<gpio_dt_spec> gpio_specs;
        gpio_specs.reserve(K_CHANNEL_COUNT);
        for(int i = 0; i < K_CHANNEL_COUNT; i++) {
            gpio_specs.push_back(
                { .port = GPIO_PORT, .pin = static_cast<gpio_pin_t>(FIRST_PIN + i), .dt_flags = GPIO_ACTIVE_HIGH }
            );
        }

        auto gpio = std::make_shared<eerie_leap::subsys::gpio::Gpio>(std::move(gpio_specs));
        zassert_ok(gpio->Initialize());

        return gpio;
    }();

    return GPIO;
}

// Fires the edge on this thread; it reaches the pipeline once the thread sleeps, at this tick.
// The emulator takes input levels only on pins configured as inputs, which TestGpio() does.
void SetInput(int channel, bool level) {
    TestGpio();
    zassert_ok(gpio_emul_input_set(GPIO_PORT, static_cast<gpio_pin_t>(FIRST_PIN + channel), level ? 1 : 0));
}

struct Record {
    int64_t uptime_ms;
    float value;
};

// The last stage: records the value of every reading that reaches it, by sensor. A probe of a
// source indicator is evaluated once per commit of it, so its records are that indicator's commit
// history. A probe of a derived indicator is also evaluated, with the old value, whenever an input
// commits while the indicator holds a change, so cases assert on its transitions.
class CommitRecorder : public IReadingProcessor {
private:
    struct Entry {
        uint32_t sensor_id_hash;
        Record record;
    };

    mutable k_mutex lock_{};
    std::vector<Entry> entries_;

public:
    CommitRecorder() {
        k_mutex_init(&lock_);
        entries_.reserve(512);
    }

    void Process(const SensorRuntime& runtime, SensorReading& reading) override {
        if(!reading.value.has_value())
            return;

        const ScopedMutex guard(lock_);
        entries_.push_back({ runtime.sensor->id_hash, { k_uptime_get(), reading.value.value() } });
    }

    [[nodiscard]] std::vector<Record> Of(std::string_view sensor_id) const {
        const uint32_t sensor_id_hash = StringHelpers::GetHash(sensor_id);

        const ScopedMutex guard(lock_);

        std::vector<Record> records;
        for(const auto& entry : entries_) {
            if(entry.sensor_id_hash == sensor_id_hash)
                records.push_back(entry.record);
        }

        return records;
    }

    // The records where the value changed, the first one included.
    [[nodiscard]] std::vector<Record> TransitionsOf(std::string_view sensor_id) const {
        std::vector<Record> transitions;
        for(const auto& record : Of(sensor_id)) {
            if(transitions.empty() || transitions.back().value != record.value)
                transitions.push_back(record);
        }

        return transitions;
    }

    [[nodiscard]] size_t CountOf(std::string_view sensor_id, float value) const {
        return static_cast<size_t>(std::ranges::count_if(Of(sensor_id), [value](const Record& record) {
            return record.value == value;
        }));
    }
};

// Fails the readings of one sensor while set, as a reader that cannot read.
class FaultInjector : public IReadingProcessor {
public:
    uint32_t sensor_id_hash = 0;
    atomic_t is_failing = ATOMIC_INIT(0);

    void Process(const SensorRuntime& runtime, SensorReading& reading) override {
        if(atomic_get(&is_failing) != 0 && runtime.sensor->id_hash == sensor_id_hash)
            reading.SetError(ReadingError::READER_FAILED);
    }
};

// Offsets from one start, in milliseconds. Absolute wake-ups: a relative sleep is rounded up by a
// tick, so edges and checks would drift off the planned ticks.
struct Clock {
    int64_t start_ticks = k_uptime_ticks();

    void SleepUntil(int offset_ms) const {
        k_sleep(K_TIMEOUT_ABS_TICKS(start_ticks + static_cast<int64_t>(k_ms_to_ticks_ceil64(offset_ms))));
    }

    [[nodiscard]] int64_t Ms(int offset_ms) const {
        return static_cast<int64_t>(k_ticks_to_ms_floor64(
            static_cast<uint64_t>(start_ticks + static_cast<int64_t>(k_ms_to_ticks_ceil64(offset_ms)))
        ));
    }
};

// The processing service over the emulated GPIO, with the recorder last. Initialized, not started.
struct ConditioningRig {
    std::shared_ptr<FsService> fs_service;
    ProcessingRig processing;
    std::shared_ptr<CommitRecorder> recorder;

    [[nodiscard]] float Value(std::string_view id) const {
        return processing.frame->TryGetReadingValue(id).value_or(NAN);
    }

    [[nodiscard]] std::optional<SensorReading> Reading(std::string_view id) const {
        return processing.frame->TryGetReading(id);
    }

    // Restarts on the stored configuration, as SensorsController does after an apply.
    void Apply(const std::vector<std::shared_ptr<Sensor>>& sensors) {
        zassert_true(processing.sensors_configuration_manager->Update(sensors), "The sensors configuration is valid");
        zassert_true(processing.processing_service->Stop());
        zassert_true(processing.processing_service->Start());
    }
};

ConditioningRig MakeRig(
    const std::vector<std::shared_ptr<Sensor>>& sensors,
    const std::shared_ptr<FaultInjector>& fault_injector = nullptr,
    std::shared_ptr<FsService> fs_service = nullptr
) {

    ConditioningRig rig;
    rig.fs_service = fs_service != nullptr ? std::move(fs_service) : MakeFormattedFsService();
    rig.processing = MakeProcessingRig(rig.fs_service, nullptr, nullptr, sensors, 0, TestGpio());

    if(fault_injector != nullptr)
        zassert_true(rig.processing.processing_service->RegisterReadingProcessor(fault_injector));

    rig.recorder = std::make_shared<CommitRecorder>();
    zassert_true(rig.processing.processing_service->RegisterReadingProcessor(rig.recorder));

    return rig;
}

void AssertTransitions(
    const std::vector<Record>& transitions,
    std::initializer_list<Record> expected,
    const char* sensor_id
) {
    std::string actual;
    for(const auto& record : transitions)
        actual += " " + std::to_string(static_cast<int>(record.value)) + "@" + std::to_string(record.uptime_ms);

    zassert_equal(transitions.size(), expected.size(), "%s transitions:%s", sensor_id, actual.c_str());

    size_t i = 0;
    for(const auto& record : expected) {
        zassert_equal(transitions[i].value, record.value, "%s transition %zu:%s", sensor_id, i, actual.c_str());
        zassert_equal(transitions[i].uptime_ms, record.uptime_ms, "%s transition %zu:%s", sensor_id, i, actual.c_str());
        i++;
    }
}

} // namespace

ZTEST(indicator_conditioning, test_without_conditioning_an_edge_is_committed_at_once) {
    SetInput(kChAtOnce, false);
    const auto rig = MakeRig({ MakeIndicator("at_once", kChAtOnce), MakeProbe("at_once_probe", "at_once") });

    const Clock clock;
    zassert_true(rig.processing.processing_service->Start());
    zassert_is_null(ConditionerOf(*rig.processing.processing_service->GetGeneration(), "at_once"));

    clock.SleepUntil(20);
    zassert_equal(rig.Value("at_once"), 0.0F, "Seeded with the level at start");

    SetInput(kChAtOnce, true);
    clock.SleepUntil(40);
    zassert_equal(rig.Value("at_once"), 1.0F);

    AssertTransitions(
        rig.recorder->TransitionsOf("at_once_probe"), { { clock.Ms(0), 0.0F }, { clock.Ms(20), 1.0F } }, "at_once_probe"
    );

    zassert_true(rig.processing.processing_service->Stop());
}

ZTEST(indicator_conditioning, test_with_debounce_0_a_toggle_flips_at_once) {
    SetInput(kChToggleAtOnce, false);
    const auto rig = MakeRig(
        {
            MakeIndicator("toggle_at_once", kChToggleAtOnce, 0, IndicatorInputMode::Toggle),
            MakeProbe("toggle_at_once_probe", "toggle_at_once"),
        }
    );

    const Clock clock;
    zassert_true(rig.processing.processing_service->Start());

    clock.SleepUntil(20);
    zassert_equal(rig.Value("toggle_at_once"), 0.0F);

    SetInput(kChToggleAtOnce, true);
    clock.SleepUntil(40);
    zassert_equal(rig.Value("toggle_at_once"), 1.0F, "Pressed");

    SetInput(kChToggleAtOnce, false);
    clock.SleepUntil(60);
    zassert_equal(rig.Value("toggle_at_once"), 1.0F, "Released: the switch holds");

    SetInput(kChToggleAtOnce, true);
    clock.SleepUntil(80);
    zassert_equal(rig.Value("toggle_at_once"), 0.0F, "Pressed again");

    AssertTransitions(
        rig.recorder->TransitionsOf("toggle_at_once_probe"),
        { { clock.Ms(0), 0.0F }, { clock.Ms(20), 1.0F }, { clock.Ms(60), 0.0F } }, "toggle_at_once_probe"
    );
    zassert_equal(rig.recorder->CountOf("toggle_at_once_probe", 1.0F), 2, "The release committed the unchanged switch");

    zassert_true(rig.processing.processing_service->Stop());
}

ZTEST(indicator_conditioning, test_an_edge_shorter_than_the_window_is_not_committed) {
    SetInput(kChShortEdge, false);
    const auto rig = MakeRig(
        { MakeIndicator("short_edge", kChShortEdge, 100), MakeProbe("short_edge_probe", "short_edge") }
    );

    const Clock clock;
    zassert_true(rig.processing.processing_service->Start());

    clock.SleepUntil(20);
    SetInput(kChShortEdge, true);
    clock.SleepUntil(60);
    zassert_equal(rig.Value("short_edge"), 0.0F);

    SetInput(kChShortEdge, false);
    clock.SleepUntil(300);
    zassert_equal(rig.Value("short_edge"), 0.0F);
    zassert_equal(rig.Reading("short_edge")->raw_value.value(), 0.0F, "The release was committed as read");
    zassert_equal(rig.recorder->CountOf("short_edge_probe", 1.0F), 0, "The pulse never reached the frame");

    zassert_true(rig.processing.processing_service->Stop());
}

ZTEST(indicator_conditioning, test_an_edge_that_holds_is_committed_once_at_the_end_of_the_window) {
    SetInput(kChHeldEdge, false);
    const auto rig = MakeRig(
        { MakeIndicator("held_edge", kChHeldEdge, 100), MakeProbe("held_edge_probe", "held_edge") }
    );

    const Clock clock;
    zassert_true(rig.processing.processing_service->Start());

    clock.SleepUntil(20);
    SetInput(kChHeldEdge, true);
    const auto edge_time = rig.processing.time_service->GetCurrentTime();

    clock.SleepUntil(70);
    zassert_equal(rig.Value("held_edge"), 0.0F, "Half the window");

    clock.SleepUntil(170);
    zassert_equal(rig.Value("held_edge"), 1.0F);
    const auto committed = rig.Reading("held_edge");
    zassert_equal(committed->source, ReadingSource::ISR, "The held reading is the one committed");
    zassert_equal(committed->raw_value.value(), 1.0F);

    // Stamped when it settled, not when the edge came.
    const auto stamped_after_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(committed->timestamp.value() - edge_time).count();
    zassert_true(
        stamped_after_ms >= 100, "Stamped %lld ms after the edge, the window is 100 ms",
        static_cast<long long>(stamped_after_ms)
    );

    AssertTransitions(
        rig.recorder->TransitionsOf("held_edge_probe"), { { clock.Ms(0), 0.0F }, { clock.Ms(120), 1.0F } },
        "held_edge_probe"
    );
    zassert_equal(rig.recorder->CountOf("held_edge_probe", 1.0F), 1, "Committed once");

    zassert_true(rig.processing.processing_service->Stop());
}

// Sampled every tick: the sample that first reads the change starts the window, later samples of
// it do not restart it, and the change commits at its end.
ZTEST(indicator_conditioning, test_a_polled_change_is_committed_once_at_the_end_of_its_window) {
    SetInput(kChPolled, false);
    const auto rig = MakeRig(
        {
            MakeIndicator("polled", kChPolled, 100, IndicatorInputMode::Level, 10),
            MakeProbe("polled_probe", "polled"),
        }
    );

    const Clock clock;
    zassert_true(rig.processing.processing_service->Start());

    clock.SleepUntil(30);
    SetInput(kChPolled, true);
    clock.SleepUntil(80);
    SetInput(kChPolled, false);

    clock.SleepUntil(250);
    zassert_equal(rig.Value("polled"), 0.0F);
    zassert_equal(rig.recorder->CountOf("polled_probe", 1.0F), 0, "A 50 ms pulse is not committed");

    clock.SleepUntil(300);
    SetInput(kChPolled, true);

    clock.SleepUntil(350);
    zassert_equal(rig.Value("polled"), 0.0F);

    clock.SleepUntil(450);
    zassert_equal(rig.Value("polled"), 1.0F);

    const auto transitions = rig.recorder->TransitionsOf("polled_probe");
    zassert_equal(transitions.size(), 2, "One change");
    zassert_equal(transitions[0].value, 0.0F);
    zassert_equal(transitions[1].value, 1.0F);
    zassert_true(
        transitions[1].uptime_ms >= clock.Ms(400) && transitions[1].uptime_ms <= clock.Ms(410),
        "Committed at %lld ms, the window ends at %lld ms", static_cast<long long>(transitions[1].uptime_ms),
        static_cast<long long>(clock.Ms(400))
    );

    zassert_true(rig.processing.processing_service->Stop());
}

// Each bounce back to the committed level cancels the window, so the change counts once the
// contact has been quiet for the window, from its last edge.
ZTEST(indicator_conditioning, test_bounce_around_an_edge_commits_one_change) {
    SetInput(kChBounce, false);
    const auto rig = MakeRig({ MakeIndicator("bounce", kChBounce, 50), MakeProbe("bounce_probe", "bounce") });

    const Clock clock;
    zassert_true(rig.processing.processing_service->Start());

    int offset_ms = 20;
    for(const bool level : { true, false, true, false, true }) {
        clock.SleepUntil(offset_ms);
        SetInput(kChBounce, level);
        offset_ms += 10;
    }

    // The last edge was at 60.
    clock.SleepUntil(100);
    zassert_equal(rig.Value("bounce"), 0.0F);
    clock.SleepUntil(140);
    zassert_equal(rig.Value("bounce"), 1.0F);

    offset_ms = 200;
    for(const bool level : { false, true, false }) {
        clock.SleepUntil(offset_ms);
        SetInput(kChBounce, level);
        offset_ms += 10;
    }

    // The last edge was at 220.
    clock.SleepUntil(260);
    zassert_equal(rig.Value("bounce"), 1.0F);
    clock.SleepUntil(300);
    zassert_equal(rig.Value("bounce"), 0.0F);

    AssertTransitions(
        rig.recorder->TransitionsOf("bounce_probe"),
        { { clock.Ms(0), 0.0F }, { clock.Ms(110), 1.0F }, { clock.Ms(270), 0.0F } }, "bounce_probe"
    );

    zassert_true(rig.processing.processing_service->Stop());
}

ZTEST(indicator_conditioning, test_the_first_value_of_a_generation_is_committed_at_once) {
    SetInput(kChFirstValue, true);
    const auto rig = MakeRig(
        { MakeIndicator("first_value", kChFirstValue, 100), MakeProbe("first_value_probe", "first_value") }
    );

    const Clock clock;
    zassert_true(rig.processing.processing_service->Start());

    clock.SleepUntil(20);
    zassert_equal(rig.Value("first_value"), 1.0F, "Nothing to settle against");
    AssertTransitions(rig.recorder->TransitionsOf("first_value_probe"), { { clock.Ms(0), 1.0F } }, "first_value_probe");

    zassert_true(rig.processing.processing_service->Stop());
    SetInput(kChFirstValue, false);
}

ZTEST(indicator_conditioning, test_toggle_flips_on_each_settled_press_and_holds) {
    SetInput(kChToggle, false);
    const auto rig = MakeRig(
        {
            MakeIndicator("toggle", kChToggle, 30, IndicatorInputMode::Toggle),
            MakeProbe("toggle_probe", "toggle"),
        }
    );

    const Clock clock;
    zassert_true(rig.processing.processing_service->Start());

    clock.SleepUntil(20);
    SetInput(kChToggle, true);
    clock.SleepUntil(40);
    zassert_equal(rig.Value("toggle"), 0.0F, "The press has not settled");
    clock.SleepUntil(70);
    zassert_equal(rig.Value("toggle"), 1.0F, "On");

    clock.SleepUntil(100);
    SetInput(kChToggle, false);
    clock.SleepUntil(160);
    zassert_equal(rig.Value("toggle"), 1.0F, "The release leaves it on");
    zassert_equal(
        rig.Reading("toggle")->raw_value.value(), 0.0F, "The release was committed, with the switch as its value"
    );

    clock.SleepUntil(200);
    SetInput(kChToggle, true);
    clock.SleepUntil(260);
    zassert_equal(rig.Value("toggle"), 0.0F, "Off");

    clock.SleepUntil(300);
    SetInput(kChToggle, false);

    // Shorter than the window.
    clock.SleepUntil(400);
    SetInput(kChToggle, true);
    clock.SleepUntil(410);
    SetInput(kChToggle, false);
    clock.SleepUntil(500);
    zassert_equal(rig.Value("toggle"), 0.0F, "A 10 ms press is ignored");

    AssertTransitions(
        rig.recorder->TransitionsOf("toggle_probe"),
        { { clock.Ms(0), 0.0F }, { clock.Ms(50), 1.0F }, { clock.Ms(230), 0.0F } }, "toggle_probe"
    );

    // The settled release at 130 committed the switch, still on.
    const auto records = rig.recorder->Of("toggle_probe");
    zassert_true(
        std::ranges::any_of(
            records,
            [&clock](const Record& record) {
                return record.uptime_ms == clock.Ms(130) && record.value == 1.0F;
            }
        ),
        "No commit at the settled release"
    );

    zassert_true(rig.processing.processing_service->Stop());
}

ZTEST(indicator_conditioning, test_a_button_held_at_start_does_not_flip_the_switch) {
    SetInput(kChHeldAtStart, true);
    const auto rig = MakeRig({ MakeIndicator("held_at_start", kChHeldAtStart, 30, IndicatorInputMode::Toggle) });

    const Clock clock;
    zassert_true(rig.processing.processing_service->Start());

    clock.SleepUntil(20);
    zassert_equal(rig.Value("held_at_start"), 0.0F, "Held at start is no press");

    SetInput(kChHeldAtStart, false);
    clock.SleepUntil(80);
    zassert_equal(rig.Value("held_at_start"), 0.0F, "The release does not flip it");

    clock.SleepUntil(100);
    SetInput(kChHeldAtStart, true);
    clock.SleepUntil(160);
    zassert_equal(rig.Value("held_at_start"), 1.0F, "The first press does");

    zassert_true(rig.processing.processing_service->Stop());
    SetInput(kChHeldAtStart, false);
}

// Polled every tick: an error says nothing about the input, so the window that runs keeps its end.
ZTEST(indicator_conditioning, test_an_error_during_a_window_neither_cancels_nor_restarts_it) {
    SetInput(kChError, false);
    const auto fault_injector = std::make_shared<FaultInjector>();
    fault_injector->sensor_id_hash = StringHelpers::GetHash("faulty");

    const auto rig = MakeRig(
        {
            MakeIndicator("faulty", kChError, 100, IndicatorInputMode::Level, 10),
            MakeProbe("faulty_probe", "faulty"),
        },
        fault_injector
    );

    const Clock clock;
    zassert_true(rig.processing.processing_service->Start());

    clock.SleepUntil(100);
    SetInput(kChError, true);

    clock.SleepUntil(130);
    atomic_set(&fault_injector->is_failing, 1);

    clock.SleepUntil(150);
    const auto failed = rig.Reading("faulty");
    zassert_true(failed.has_value() && failed->HasError(), "The error is committed");
    zassert_equal(failed->error, ReadingError::READER_FAILED);
    zassert_equal(rig.Value("faulty"), 0.0F, "The value is the last processed one");

    clock.SleepUntil(160);
    atomic_clear(&fault_injector->is_failing);

    clock.SleepUntil(190);
    zassert_equal(rig.Value("faulty"), 0.0F);

    clock.SleepUntil(220);
    zassert_equal(
        rig.Value("faulty"), 1.0F, "Settled at the end of the window that began at 100, not 100 ms after the errors"
    );
    zassert_false(rig.Reading("faulty")->HasError());

    const auto transitions = rig.recorder->TransitionsOf("faulty_probe");
    zassert_equal(transitions.size(), 2);
    zassert_equal(transitions[1].value, 1.0F);
    zassert_true(
        transitions[1].uptime_ms >= clock.Ms(200) && transitions[1].uptime_ms <= clock.Ms(210),
        "Committed at %lld ms, the window ends at %lld ms", static_cast<long long>(transitions[1].uptime_ms),
        static_cast<long long>(clock.Ms(200))
    );

    zassert_true(rig.processing.processing_service->Stop());
}

// A dependent is evaluated on commits only, so what the conditioner holds never reaches it.
ZTEST(indicator_conditioning, test_a_dependent_sees_only_settled_values) {
    SetInput(kChFollowed, false);
    const auto rig = MakeRig(
        {
            MakeIndicator("followed", kChFollowed, 100),
            MakeDerivedIndicator("follower", "followed"),
        }
    );

    const Clock clock;
    zassert_true(rig.processing.processing_service->Start());

    // A bounce that comes back, then a 50 ms press.
    int offset_ms = 20;
    for(const bool level : { true, false, true, false }) {
        clock.SleepUntil(offset_ms);
        SetInput(kChFollowed, level);
        offset_ms += 10;
    }

    clock.SleepUntil(300);
    SetInput(kChFollowed, true);
    clock.SleepUntil(350);
    SetInput(kChFollowed, false);

    clock.SleepUntil(550);
    zassert_equal(rig.Value("follower"), 0.0F);
    zassert_equal(rig.recorder->CountOf("follower", 1.0F), 0, "Neither reached the follower");

    clock.SleepUntil(600);
    SetInput(kChFollowed, true);
    clock.SleepUntil(650);
    zassert_equal(rig.Value("follower"), 0.0F);
    clock.SleepUntil(750);
    zassert_equal(rig.Value("follower"), 1.0F);

    AssertTransitions(
        rig.recorder->TransitionsOf("follower"), { { clock.Ms(0), 0.0F }, { clock.Ms(700), 1.0F } }, "follower"
    );

    zassert_true(rig.processing.processing_service->Stop());
}

// A held dependent keeps its value; its own work item settles it, and that commit evaluates the
// sensors that read it.
ZTEST(indicator_conditioning, test_a_derived_indicator_settles_on_its_own_window) {
    SetInput(kChContact, false);
    const auto rig = MakeRig(
        {
            MakeIndicator("contact", kChContact),
            MakeDerivedIndicator("moving", "contact", 100),
            MakeProbe("moving_probe", "moving"),
        }
    );

    const Clock clock;
    zassert_true(rig.processing.processing_service->Start());

    clock.SleepUntil(20);
    SetInput(kChContact, true);

    clock.SleepUntil(70);
    zassert_equal(rig.Value("contact"), 1.0F);
    zassert_equal(rig.Value("moving"), 0.0F);

    clock.SleepUntil(170);
    zassert_equal(rig.Value("moving"), 1.0F);
    zassert_equal(rig.Reading("moving")->source, ReadingSource::PROCESSING);

    AssertTransitions(
        rig.recorder->TransitionsOf("moving_probe"), { { clock.Ms(0), 0.0F }, { clock.Ms(120), 1.0F } }, "moving_probe"
    );

    zassert_true(rig.processing.processing_service->Stop());
}

// Stop() cancels the held change itself: the case keeps the generation, and so the conditioner,
// alive past Stop(), so a settle that was not cancelled would still commit into the frame.
ZTEST(indicator_conditioning, test_Stop_drops_a_held_change) {
    SetInput(kChStop, false);
    const auto rig = MakeRig({ MakeIndicator("stopped", kChStop, 100), MakeProbe("stopped_probe", "stopped") });

    const Clock clock;
    zassert_true(rig.processing.processing_service->Start());

    clock.SleepUntil(20);
    SetInput(kChStop, true);

    clock.SleepUntil(50);
    auto generation = rig.processing.processing_service->GetGeneration();
    const size_t records = rig.recorder->Of("stopped_probe").size();
    zassert_true(rig.processing.processing_service->Stop());

    clock.SleepUntil(200);
    zassert_false(rig.Reading("stopped").has_value(), "Nothing was committed after the stop");
    zassert_equal(rig.recorder->Of("stopped_probe").size(), records, "No dependent was evaluated after the stop");
    generation.reset();

    zassert_true(rig.processing.processing_service->Start());
    clock.SleepUntil(220);
    zassert_equal(rig.Value("stopped"), 1.0F, "The first value of the next generation is committed at once");

    zassert_true(rig.processing.processing_service->Stop());
    SetInput(kChStop, false);
}

// A pause stops the window of a held change with the sources, since none can confirm or cancel it
// until Resume(), and Resume() runs what was left of it. The level read again at Resume()
// confirms or cancels the change as any reading does.
ZTEST(indicator_conditioning, test_a_pause_stops_the_window_of_a_held_change) {
    SetInput(kChPause, false);
    const auto rig = MakeRig(
        {
            MakeIndicator("paused", kChPause, 100, IndicatorInputMode::Toggle),
            MakeProbe("paused_probe", "paused"),
        }
    );

    const Clock clock;
    zassert_true(rig.processing.processing_service->Start());

    // A press released while paused does not count, although the pause outlasts its window.
    clock.SleepUntil(20);
    SetInput(kChPause, true);
    clock.SleepUntil(50);
    zassert_true(rig.processing.processing_service->Pause());
    clock.SleepUntil(100);
    SetInput(kChPause, false);
    clock.SleepUntil(200);
    zassert_true(rig.processing.processing_service->Resume());

    clock.SleepUntil(350);
    zassert_equal(rig.Value("paused"), 0.0F, "The press in flight at the pause did not flip the switch");

    // A press still held at Resume() counts once the 70 ms left of its window have passed.
    clock.SleepUntil(400);
    SetInput(kChPause, true);
    clock.SleepUntil(430);
    zassert_true(rig.processing.processing_service->Pause());
    clock.SleepUntil(500);
    zassert_true(rig.processing.processing_service->Resume());

    clock.SleepUntil(560);
    zassert_equal(rig.Value("paused"), 0.0F);
    clock.SleepUntil(580);
    zassert_equal(rig.Value("paused"), 1.0F, "Settled 70 ms after Resume()");

    AssertTransitions(
        rig.recorder->TransitionsOf("paused_probe"), { { clock.Ms(0), 0.0F }, { clock.Ms(570), 1.0F } }, "paused_probe"
    );

    zassert_true(rig.processing.processing_service->Stop());
    SetInput(kChPause, false);
}

// A slow polled switch and a Level sensor: only the switch has a reading before its first sample,
// the one Start() commits with its dependents.
ZTEST(indicator_conditioning, test_a_switch_is_committed_before_its_input_has_a_reading) {
    SetInput(kChSwitchAtStart, false);
    SetInput(kChLevelAtStart, false);
    const auto rig = MakeRig(
        {
            MakeIndicator("switch_at_start", kChSwitchAtStart, 30, IndicatorInputMode::Toggle, 1000),
            MakeProbe("switch_at_start_probe", "switch_at_start"),
            MakeIndicator("level_at_start", kChLevelAtStart, 30, IndicatorInputMode::Level, 1000),
        }
    );

    const Clock clock;
    zassert_true(rig.processing.processing_service->Start());

    // When Start() returns, and later, still before the first sample at 1000.
    for(const int offset_ms : { 0, 100 }) {
        if(offset_ms > 0)
            clock.SleepUntil(offset_ms);

        const auto reading = rig.Reading("switch_at_start");
        zassert_true(reading.has_value(), "No reading at %d ms", offset_ms);
        zassert_equal(reading->value.value(), 0.0F);
        zassert_equal(reading->status, ReadingStatus::PROCESSED);
        zassert_equal(reading->source, ReadingSource::PROCESSING);
        zassert_false(reading->raw_value.has_value(), "No sample yet: the reading is the start's");
        zassert_equal(rig.Value("switch_at_start_probe"), 0.0F, "The start's commit evaluated the dependent");

        zassert_false(rig.Reading("level_at_start").has_value(), "A Level sensor waits for its input");
    }

    zassert_true(rig.processing.processing_service->Stop());
}

// Restarts with the same switch, edge-driven, and a polled one whose sampling rate changes, which
// is not part of what defines a switch; an unrelated sensor is added.
ZTEST(indicator_conditioning, test_a_sensors_apply_keeps_a_switch_that_is_unchanged) {
    SetInput(kChKeepEdge, false);
    SetInput(kChKeepPolled, false);
    auto rig = MakeRig(
        {
            MakeIndicator("keep_edge", kChKeepEdge, 30, IndicatorInputMode::Toggle),
            MakeIndicator("keep_polled", kChKeepPolled, 30, IndicatorInputMode::Toggle, 100),
            MakeProbe("keep_polled_probe", "keep_polled"),
        }
    );

    const Clock clock;
    zassert_true(rig.processing.processing_service->Start());

    // Each pressed and released. The polled one is pressed after its first sample at 100, which
    // would otherwise take the press as the level at start; it reads the press at 200 and the
    // release at 300.
    clock.SleepUntil(20);
    SetInput(kChKeepEdge, true);
    clock.SleepUntil(120);
    SetInput(kChKeepEdge, false);
    SetInput(kChKeepPolled, true);
    clock.SleepUntil(260);
    SetInput(kChKeepPolled, false);
    clock.SleepUntil(400);
    zassert_equal(rig.Value("keep_edge"), 1.0F);
    zassert_equal(rig.Value("keep_polled"), 1.0F);

    rig.Apply(
        {
            MakeIndicator("keep_edge", kChKeepEdge, 30, IndicatorInputMode::Toggle),
            MakeIndicator("keep_polled", kChKeepPolled, 30, IndicatorInputMode::Toggle, 1000),
            MakeProbe("keep_polled_probe", "keep_polled"),
            MakeVirtualSensor("keep_other", "1 + 1", 1000),
        }
    );

    const Clock restart;
    zassert_equal(rig.Value("keep_edge"), 1.0F, "Still on when Start() returns");
    zassert_equal(rig.Value("keep_polled"), 1.0F);
    zassert_equal(rig.Value("keep_polled_probe"), 1.0F, "The start's commit evaluated the dependent");

    restart.SleepUntil(100);
    const auto edge = rig.Reading("keep_edge");
    zassert_equal(edge->value.value(), 1.0F, "The seed of the released button does not change it");
    zassert_equal(edge->raw_value.value_or(-1.0F), 0.0F, "The seed came after the start's commit");
    const auto polled = rig.Reading("keep_polled");
    zassert_equal(polled->value.value(), 1.0F);
    zassert_equal(polled->source, ReadingSource::PROCESSING);
    zassert_false(polled->raw_value.has_value(), "Before its first sample, from the start's commit");
    zassert_equal(rig.Value("keep_polled_probe"), 1.0F);

    // The next press turns it off.
    SetInput(kChKeepEdge, true);
    restart.SleepUntil(160);
    zassert_equal(rig.Value("keep_edge"), 0.0F);

    zassert_true(rig.processing.processing_service->Stop());
    SetInput(kChKeepEdge, false);
}

ZTEST(indicator_conditioning, test_a_sensors_apply_resets_a_switch_whose_conditioning_changed) {
    SetInput(kChReset, false);
    SetInput(kChResetInput, false);

    const auto switch_on = [](ConditioningRig& rig, uint32_t debounce_ms) {
        const Clock clock;
        clock.SleepUntil(20);
        SetInput(kChReset, true);
        clock.SleepUntil(40 + static_cast<int>(debounce_ms));
        SetInput(kChReset, false);
        clock.SleepUntil(60 + 2 * static_cast<int>(debounce_ms));
        zassert_equal(rig.Value("reset"), 1.0F, "Switched on");
    };

    auto rig = MakeRig(
        {
            MakeIndicator("reset", kChReset, 30, IndicatorInputMode::Toggle),
            MakeIndicator("reset_input", kChResetInput),
        }
    );
    zassert_true(rig.processing.processing_service->Start());
    switch_on(rig, 30);

    rig.Apply(
        {
            MakeIndicator("reset", kChReset, 40, IndicatorInputMode::Toggle),
            MakeIndicator("reset_input", kChResetInput),
        }
    );
    zassert_equal(rig.Value("reset"), 0.0F, "Another debounce starts off");

    switch_on(rig, 40);

    // A sensor that stops being a switch, or leaves the configuration, leaves no switch behind:
    // back exactly as it was, it starts off.
    rig.Apply(
        {
            MakeIndicator("reset", kChReset, 40, IndicatorInputMode::Level),
            MakeIndicator("reset_input", kChResetInput),
        }
    );
    rig.Apply(
        {
            MakeIndicator("reset", kChReset, 40, IndicatorInputMode::Toggle),
            MakeIndicator("reset_input", kChResetInput),
        }
    );
    zassert_equal(rig.Value("reset"), 0.0F, "A switch that was Level in between starts off");

    switch_on(rig, 40);

    rig.Apply({ MakeIndicator("reset_input", kChResetInput) });
    rig.Apply(
        {
            MakeIndicator("reset", kChReset, 40, IndicatorInputMode::Toggle),
            MakeIndicator("reset_input", kChResetInput),
        }
    );
    zassert_equal(rig.Value("reset"), 0.0F, "A switch that was removed in between starts off");

    switch_on(rig, 40);

    rig.Apply(
        {
            MakeDerivedIndicator("reset", "reset_input", 40, IndicatorInputMode::Toggle),
            MakeIndicator("reset_input", kChResetInput),
        }
    );
    zassert_equal(rig.Value("reset"), 0.0F, "Another type starts off");

    const Clock clock;
    clock.SleepUntil(50);
    zassert_equal(rig.Value("reset"), 0.0F);

    zassert_true(rig.processing.processing_service->Stop());
}

ZTEST(indicator_conditioning, test_a_new_service_starts_every_switch_off) {
    SetInput(kChReboot, false);
    const std::vector<std::shared_ptr<Sensor>> sensors{
        MakeIndicator("reboot", kChReboot, 0, IndicatorInputMode::Toggle)
    };

    const auto before = MakeRig(sensors);
    zassert_true(before.processing.processing_service->Start());

    const Clock clock;
    clock.SleepUntil(20);
    SetInput(kChReboot, true);
    clock.SleepUntil(40);
    SetInput(kChReboot, false);
    clock.SleepUntil(60);
    zassert_equal(before.Value("reboot"), 1.0F);
    zassert_true(before.processing.processing_service->Stop());

    // The same stored configuration in a new service, as after a reboot.
    const auto after = MakeRig(sensors, nullptr, before.fs_service);
    zassert_true(after.processing.processing_service->Start());
    clock.SleepUntil(80);
    zassert_equal(after.Value("reboot"), 0.0F, "A switch does not survive a reboot");

    zassert_true(after.processing.processing_service->Stop());
}

namespace {

using eerie_leap::domain::canbus_domain::configuration::CanbusConfigurationManager;
using eerie_leap::domain::canbus_domain::models::CanbusConfiguration;
using eerie_leap::domain::canbus_domain::models::CanChannelConfiguration;
using eerie_leap::domain::canbus_domain::models::CanMessageConfiguration;
using eerie_leap::domain::canbus_domain::models::CanSignalConfiguration;
using eerie_leap::domain::canbus_domain::services::CanbusService;
using eerie_leap::domain::sensor_domain::models::sources::CanbusSource;
using eerie_leap::subsys::canbus::CanbusProxy;
using eerie_leap::subsys::canbus::CanbusType;
using eerie_leap::subsys::canbus::CanId;

constexpr uint8_t CAN_BUS = 0;
constexpr uint32_t CAN_FRAME_ID = 0x300;

const device* CanLoopbackDevice() {
    return DEVICE_DT_GET(DT_NODELABEL(can_loopback0));
}

// A failing assertion skips the destructors, so the controller is stopped for the next test.
void StopCanLoopbackDevice(void*) {
    can_stop(CanLoopbackDevice());
}

// One classical channel with an 8-byte message whose first byte is the signal "sig".
std::shared_ptr<CanbusConfigurationManager> MakeCanbusConfigurationManager(std::shared_ptr<FsService> fs_service) {
    CanSignalConfiguration signal(std::allocator_arg, Mrm::GetDefaultPmr());
    signal.start_bit = 0;
    signal.size_bits = 8;
    signal.is_signed = false;
    signal.SetName("sig");

    auto message = std::make_shared<CanMessageConfiguration>(std::allocator_arg, Mrm::GetDefaultPmr());
    message->frame_id = CAN_FRAME_ID;
    message->is_enabled = true;
    message->name = "EL_SWITCH";
    message->message_size = 8;
    message->signal_configurations.push_back(std::move(signal));

    CanChannelConfiguration channel(std::allocator_arg, Mrm::GetDefaultPmr());
    channel.type = CanbusType::CLASSICAL_CAN;
    channel.bus_channel = CAN_BUS;
    channel.bitrate = 500000;
    channel.message_configurations.push_back(std::move(message));

    CanbusConfiguration configuration(std::allocator_arg, Mrm::GetDefaultPmr());
    configuration.channel_configurations.emplace(CAN_BUS, std::move(channel));

    auto manager = std::make_shared<CanbusConfigurationManager>(
        std::make_unique<CborConfigurationService<CborCanbusConfig>>("canbus_config", std::move(fs_service)), nullptr
    );
    zassert_true(manager->Update(configuration), "The CAN configuration is valid");

    return manager;
}

std::shared_ptr<Sensor> MakeCanIndicator(std::string_view id, uint32_t debounce_ms) {
    auto sensor = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), id);
    sensor->configuration.type = SensorType::CANBUS_INDICATOR;
    sensor->configuration.canbus_source = make_unique_pmr<CanbusSource>(
        Mrm::GetDefaultPmr(), CAN_BUS, CAN_FRAME_ID, "sig"
    );
    sensor->configuration.UpdateConnectionString();
    sensor->configuration.debounce_ms = debounce_ms;

    return sensor;
}

// The processing service over the loopback CAN controller alone, with the recorder last.
// Initialized, not started.
struct CanConditioningRig : ConditioningRig {
    std::shared_ptr<CanbusService> canbus_service;
    std::shared_ptr<CanbusProxy> canbus;

    // On this thread; the frame reaches the pipeline once the thread sleeps, at this tick.
    void Send(uint8_t signal) const {
        const std::array<uint8_t, 8> data{ signal };
        zassert_ok((*canbus)->SendFrame(CanId::Standard(CAN_FRAME_ID), data, K_MSEC(100)));
    }
};

CanConditioningRig MakeCanRig(const std::vector<std::shared_ptr<Sensor>>& sensors) {
    CanConditioningRig rig;
    rig.fs_service = MakeFormattedFsService();

    rig.canbus_service = std::make_shared<CanbusService>(
        [](uint8_t bus_channel) {
            return bus_channel == CAN_BUS ? CanLoopbackDevice() : nullptr;
        },
        MakeCanbusConfigurationManager(rig.fs_service)
    );
    rig.canbus = rig.canbus_service->GetCanbus(CAN_BUS);
    zassert_not_null(rig.canbus.get(), "The channel is open");

    // The service opens the channel in normal mode, in which the loopback controller hands no
    // frame back.
    zassert_ok(can_stop(CanLoopbackDevice()));
    zassert_ok(can_set_mode(CanLoopbackDevice(), CAN_MODE_LOOPBACK));
    zassert_ok(can_start(CanLoopbackDevice()));

    auto& processing = rig.processing;
    processing.time_service = MakeTimeService();
    processing.frame = std::make_shared<SensorReadingsFrame>();
    processing.sensors_configuration_manager = std::make_shared<SensorsConfigurationManager>(
        std::make_unique<CborConfigurationService<CborSensorsConfig>>("sensors_config", rig.fs_service), nullptr, 0, 0
    );
    zassert_true(processing.sensors_configuration_manager->Update(sensors), "The sensors configuration is valid");

    processing.processing_service = std::make_shared<SensorsProcessingService>(
        processing.sensors_configuration_manager, processing.frame, processing.time_service, nullptr, nullptr, nullptr,
        rig.canbus_service
    );
    zassert_true(processing.processing_service->Initialize());

    rig.recorder = std::make_shared<CommitRecorder>();
    zassert_true(processing.processing_service->RegisterReadingProcessor(rig.recorder));

    return rig;
}

} // namespace

ZTEST_SUITE(indicator_conditioning_can, NULL, NULL, NULL, StopCanLoopbackDevice, NULL);

// A CAN signal reaches the pipeline only with a frame, so a change in the last frame settles on
// the conditioner's work item. A pause detaches the routes and stops the window; Resume() runs
// what was left of it, since no frame will confirm the change.
ZTEST(indicator_conditioning_can, test_a_can_signal_that_stops_changing_settles_at_the_end_of_its_window) {
    const auto rig = MakeCanRig({ MakeCanIndicator("can_signal", 100), MakeProbe("can_signal_probe", "can_signal") });

    const Clock clock;
    zassert_true(rig.processing.processing_service->Start());

    clock.SleepUntil(10);
    rig.Send(0);
    clock.SleepUntil(20);
    rig.Send(1);

    clock.SleepUntil(110);
    zassert_equal(rig.Value("can_signal"), 0.0F, "Held");
    clock.SleepUntil(170);
    zassert_equal(rig.Value("can_signal"), 1.0F, "Settled without another frame");

    // Paused 30 ms into the window of the next change, which leaves 70 ms of it.
    clock.SleepUntil(200);
    rig.Send(0);
    clock.SleepUntil(230);
    zassert_true(rig.processing.processing_service->Pause());
    clock.SleepUntil(330);
    zassert_true(rig.processing.processing_service->Resume());

    clock.SleepUntil(390);
    zassert_equal(rig.Value("can_signal"), 1.0F);
    clock.SleepUntil(410);
    zassert_equal(rig.Value("can_signal"), 0.0F, "Settled 70 ms after Resume()");

    AssertTransitions(
        rig.recorder->TransitionsOf("can_signal_probe"),
        { { clock.Ms(10), 0.0F }, { clock.Ms(120), 1.0F }, { clock.Ms(400), 0.0F } }, "can_signal_probe"
    );
    zassert_equal(rig.recorder->CountOf("can_signal_probe", 1.0F), 1, "Committed once");

    zassert_true(rig.processing.processing_service->Stop());
}

#endif // CONFIG_BOARD_NATIVE_SIM
