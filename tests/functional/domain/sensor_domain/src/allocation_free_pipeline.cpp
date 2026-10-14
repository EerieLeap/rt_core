// Guards the rule the sensor pipeline is built on: allocate when the configuration changes,
// never when a sample arrives. Counts every heap allocation in the process while a sample goes
// from the ADC and a reader through the processors into the frame and out to a consumer, while a
// conditioned indicator holds a change and settles it from its work item, and while the sensor
// events publisher takes it and the bus delivers it to a binding.

#include <array>
#include <memory>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"
#include "subsys/time/rtc_provider.h"
#include "subsys/time/boot_elapsed_time_provider.h"
#include "subsys/time/time_service.h"
#include "subsys/threading/work_queue_thread.h"
#include "subsys/gpio/gpio_simulator.h"
#include "subsys/canbus/can_frame.h"
#include "subsys/expression_engine/expression_evaluator.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "domain/sensor_domain/sensor_readers/sensor_reader_physical_indicator.h"
#include "domain/sensor_domain/sensor_readers/sensor_reader_virtual_analog.h"
#include "domain/sensor_domain/sensor_readers/sensor_reader_factory.h"
#include "domain/sensor_domain/services/adc_sample_set.h"
#include "domain/sensor_domain/processors/expression_processor.h"
#include "domain/sensor_domain/processors/script_processor.h"
#include "domain/sensor_domain/processors/reading_pipeline.hpp"
#include "domain/sensor_domain/runtime/sensor_pipeline_builder.h"

#include "allocation_counter.h"
#include "processing_test_support.h"
#include "sensor_events_test_support.h"

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::subsys::time;
using namespace eerie_leap::subsys::threading;
using namespace eerie_leap::subsys::adc;
using namespace eerie_leap::subsys::gpio;
using namespace eerie_leap::subsys::canbus;
using namespace eerie_leap::subsys::expression_engine;
using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::sensor_domain::utilities;
using namespace eerie_leap::domain::sensor_domain::sensor_readers;
using namespace eerie_leap::domain::sensor_domain::processors;
using namespace eerie_leap::domain::sensor_domain::runtime;
using namespace eerie_leap::domain::sensor_domain::services;
using rt_core_test::GetAllocationCount;

ZTEST_SUITE(allocation_free_pipeline, NULL, NULL, NULL, NULL, NULL);

namespace {

struct Pipeline {
    std::shared_ptr<TimeService> time_service;
    std::shared_ptr<SensorReadingsFrame> frame;
    std::shared_ptr<GpioSimulator> gpio;
    std::shared_ptr<AdcEmulator> adc;
    std::vector<std::shared_ptr<Sensor>> sensors;          // indicator, virtual, raw CAN, dependent, analog
    std::shared_ptr<SensorGeneration> generation;
    std::unique_ptr<AdcSampleSet> adc_samples;             // The analog sensor's rate group.
    std::vector<std::unique_ptr<ISensorReader>> readers;   // indicator, virtual, analog
    std::vector<const SensorRuntime*> reader_runtimes;
    std::shared_ptr<std::vector<std::shared_ptr<IReadingProcessor>>> processors;
    std::shared_ptr<ReadingPipeline> pipeline;
    std::array<SensorReading, 8> buffer;
    std::array<SensorReading, 8> raw_readings;
    std::array<CanFrame, 8> raw_frames;
};

Pipeline MakePipeline() {
    Pipeline pipeline;

    auto time_service = std::make_shared<TimeService>(
        std::make_shared<BootElapsedTimeProvider>(), std::make_shared<RtcProvider>());
    pipeline.time_service = time_service;

    pipeline.frame = std::make_shared<SensorReadingsFrame>();

    pipeline.gpio = std::make_shared<GpioSimulator>();
    pipeline.gpio->Initialize();

    auto indicator = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "indicator");
    indicator->configuration.type = SensorType::PHYSICAL_INDICATOR;
    indicator->configuration.channel = 0;
    indicator->configuration.sampling_rate_ms = 100;
    indicator->configuration.expression = "x";

    auto derived = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "derived");
    derived->configuration.type = SensorType::VIRTUAL_ANALOG;
    derived->configuration.sampling_rate_ms = 100;
    derived->configuration.expression = "indicator * 2 + 1";

    auto raw = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "raw");
    raw->configuration.type = SensorType::CANBUS_RAW;

    // Evaluated after every indicator commit, without a timer.
    auto dependent = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "dependent");
    dependent->configuration.type = SensorType::VIRTUAL_ANALOG;
    dependent->configuration.expression = "indicator + derived";

    // Sampled through the emulated ADC, with its channel pinned to 1 V.
    auto analog = processing_test_support::MakeAnalogSensor("analog", 0, 100);

    pipeline.adc = processing_test_support::MakeEmulator();
    auto adc_manager = processing_test_support::MakeAdcManager(pipeline.adc);
    processing_test_support::DriveConstant(*pipeline.adc, 0, 1000);

    pipeline.sensors = { indicator, derived, raw, dependent, analog };
    // Lays out the slots and binds "indicator" in the derived expression to its value slot.
    pipeline.generation = SensorPipelineBuilder(nullptr, pipeline.frame).Build(
        std::make_shared<const std::vector<std::shared_ptr<Sensor>>>(pipeline.sensors));
    pipeline.generation->adc_configuration = processing_test_support::MakeAdcConfiguration(
        adc_manager->GetChannelCount(), 16, processing_test_support::MakeCalibrator({ {0.0F, 0.0F}, {5.0F, 5.0F} }));

    pipeline.readers.push_back(std::make_unique<SensorReaderPhysicalIndicator>(time_service, pipeline.generation->runtimes[0], pipeline.gpio));
    pipeline.reader_runtimes.push_back(&pipeline.generation->runtimes[0]);
    pipeline.readers.push_back(std::make_unique<SensorReaderVirtualAnalog>(time_service, pipeline.generation->runtimes[1]));
    pipeline.reader_runtimes.push_back(&pipeline.generation->runtimes[1]);

    // Built the way a rate group builds it: the reader reserves its slot, the set is sealed after.
    pipeline.adc_samples = std::make_unique<AdcSampleSet>();
    RateGroupBuildContext context {
        .adc_samples = *pipeline.adc_samples,
        .adc_configuration = pipeline.generation->adc_configuration.get()
    };
    const SensorRuntime* analog_runtime = pipeline.generation->Find(analog->id_hash);
    pipeline.readers.push_back(SensorReaderFactory(time_service, nullptr, adc_manager).Create(*analog_runtime, context));
    pipeline.reader_runtimes.push_back(analog_runtime);
    pipeline.adc_samples->Seal();

    pipeline.processors = std::make_shared<std::vector<std::shared_ptr<IReadingProcessor>>>();
    pipeline.processors->push_back(std::make_shared<ExpressionProcessor>(pipeline.frame));
    pipeline.processors->push_back(std::make_shared<ScriptProcessor>("post_process_sensor_value"));
    pipeline.pipeline = std::make_shared<ReadingPipeline>(pipeline.frame, pipeline.processors, time_service);

    return pipeline;
}

// One sample of every source kind, the way the services run them.
void RunSamples(Pipeline& pipeline, uint32_t iteration) {
    // The rate group's ADC channels first, then the readers in generation order with one tick;
    // committing the indicator also evaluates the dependent sensor.
    pipeline.adc_samples->Acquire();
    const TickContext tick{ pipeline.time_service->GetCurrentTime() };

    for(size_t i = 0; i < pipeline.readers.size(); i++) {
        SensorReading reading = pipeline.readers[i]->Read(tick);
        pipeline.pipeline->Process(*pipeline.reader_runtimes[i], reading);
    }

    // What a CAN reader produces for a raw sensor: the frame travels by pointer and is copied once.
    CanFrame can_frame;
    can_frame.id = 0x100 + iteration;
    can_frame.data = { 1, 2, 3, 4, 5, 6, 7, 8 };

    SensorReading raw_reading(pipeline.sensors[2].get());
    raw_reading.source = ReadingSource::ISR;
    raw_reading.timestamp = std::chrono::system_clock::time_point(std::chrono::milliseconds(k_uptime_get()));
    raw_reading.status = ReadingStatus::RAW;
    raw_reading.can_frame = &can_frame;
    pipeline.pipeline->Process(pipeline.generation->runtimes[2], raw_reading);
}

// What the renderer, the log writer, live data and the CAN frame builder do with the results.
void RunConsumers(Pipeline& pipeline) {
    (void)pipeline.frame->TakeProcessedReadings(pipeline.buffer);
    (void)pipeline.frame->SnapshotProcessedReadings(pipeline.buffer);

    CanFrame can_frame;
    (void)pipeline.frame->TryGetCanFrame(pipeline.sensors[2]->id_hash, can_frame);
    (void)pipeline.frame->TakeRawCanFrames(pipeline.raw_readings, pipeline.raw_frames);

    const std::array<uint32_t, 2> hashes = { pipeline.sensors[0]->id_hash, pipeline.sensors[1]->id_hash };
    std::array<std::optional<float>, 2> values;
    pipeline.frame->GetReadingValues(hashes, values);

    (void)pipeline.frame->TryGetReadingValue("derived");
    (void)pipeline.frame->TryGetReading(pipeline.sensors[0]->id_hash);
}

} // namespace

ZTEST(allocation_free_pipeline, test_counting_allocator_is_in_use) {
    const size_t before = GetAllocationCount();
    auto block = std::make_unique<uint8_t[]>(32);

    zassert_equal(GetAllocationCount() - before, 1, "The replaced operator new must count, or the test below proves nothing");
}

ZTEST(allocation_free_pipeline, test_steady_state_samples_do_not_allocate) {
    auto pipeline = MakePipeline();

    // First samples create the per-sensor entries and bind the expression variables.
    for(uint32_t i = 0; i < 3; i++) {
        RunSamples(pipeline, i);
        RunConsumers(pipeline);
    }

    const auto derived = pipeline.frame->TryGetReading("derived").value();
    zassert_equal(derived.status, ReadingStatus::PROCESSED, "Warm-up must have produced a processed derived value");

    const size_t allocations_before = GetAllocationCount();
    const uint32_t adc_requests_before = pipeline.adc->GetStats().requests;

    for(uint32_t i = 3; i < 103; i++) {
        RunSamples(pipeline, i);
        RunConsumers(pipeline);
    }

    zassert_equal(GetAllocationCount() - allocations_before, 0,
        "%zu allocations during 100 steady-state samples", GetAllocationCount() - allocations_before);

    // And the pipeline still produced the right results while being counted (the simulator's level is random).
    const float indicator = pipeline.frame->TryGetReadingValue("indicator").value();
    zassert_true(indicator == 0.0F || indicator == 1.0F);
    zassert_equal(pipeline.frame->TryGetReadingValue("derived").value(), indicator * 2.0F + 1.0F);
    zassert_equal(pipeline.frame->TryGetReadingValue("dependent").value(), indicator + indicator * 2.0F + 1.0F,
        "The dependent sensor was evaluated after the indicator, with the derived value of this pass");
    zassert_equal(pipeline.frame->GetDroppedRawCanFrameCount(), 0, "The log writer path drained the raw queue every sample");

    // One conversion request per tick, and the analog value came from the ADC through the calibrator.
    zassert_equal(pipeline.adc->GetStats().requests - adc_requests_before, 100);
    const auto analog = pipeline.frame->TryGetReading("analog").value();
    zassert_equal(analog.status, ReadingStatus::PROCESSED);
    const float expected_volts = pipeline.generation->adc_configuration->channel_configurations->at(0)->calibrator
        ->InterpolateToCalibratedRange(1.0F);
    zassert_within(analog.voltage.value(), expected_volts, 0.01F);
    zassert_within(analog.value.value(), analog.voltage.value() * 20.0F, 1e-3F);

    CanFrame can_frame;
    zassert_true(pipeline.frame->TryGetCanFrame(pipeline.sensors[2]->id_hash, can_frame));
    zassert_equal(can_frame.id, 0x100 + 102);
}

ZTEST(allocation_free_pipeline, test_steady_state_publishing_does_not_allocate) {
    namespace events = sensor_events_test_support;

    events::RegisterSensorEvents();

    // The sentinel is in the last slot; see sensor_events_test_support.h.
    auto a = events::MakeSensor("alloc_a");
    auto b = events::MakeSensor("alloc_b");
    auto sentinel = events::MakeSensor("alloc_sentinel");
    const std::vector<std::shared_ptr<Sensor>> sensors { a, b, sentinel };

    auto frame = std::make_shared<SensorReadingsFrame>();
    frame->Configure(sensors);

    auto a_recorder = std::make_shared<events::Recorder>();
    auto b_recorder = std::make_shared<events::Recorder>();
    auto sentinel_recorder = std::make_shared<events::Recorder>();
    auto a_binding = events::Bind(*a, a_recorder);
    auto b_binding = events::Bind(*b, b_recorder);
    auto sentinel_binding = events::Bind(*sentinel, sentinel_recorder);
    zassert_not_null(a_binding.get());
    zassert_not_null(b_binding.get());
    zassert_not_null(sentinel_binding.get());

    auto publisher = std::make_unique<SensorEventsPublisher>(frame);
    zassert_true(publisher->Initialize());
    zassert_true(publisher->Start());

    // Outside the window: the bus thread starts and the first tick caches a and the sentinel.
    zassert_true(events::Round(*publisher, *frame, { { a.get(), 0.0F } }, *sentinel, *sentinel_recorder));

    constexpr size_t k_rounds = 20;
    size_t delivered_rounds = 0;

    const size_t allocations_before = GetAllocationCount();

    for(size_t i = 0; i < k_rounds; i++) {
        const float value = static_cast<float>(i + 1);

        // b goes out for the first time here, so the window also holds one miss: Accepts() and the
        // cache's Store().
        const bool is_delivered = i == 0
            ? events::Round(*publisher, *frame, { { a.get(), value }, { b.get(), 100.0F } }, *sentinel, *sentinel_recorder)
            : events::Round(*publisher, *frame, { { a.get(), value } }, *sentinel, *sentinel_recorder);

        delivered_rounds += is_delivered ? 1 : 0;
    }

    const size_t allocations = GetAllocationCount() - allocations_before;

    publisher->Stop();

    zassert_equal(allocations, 0, "%zu allocations during %zu published rounds", allocations, k_rounds);
    zassert_equal(delivered_rounds, k_rounds);
    zassert_equal(sentinel_recorder->Count(), 1 + k_rounds);
    zassert_equal(a_recorder->Count(), 1 + k_rounds);
    zassert_equal(a_recorder->At(k_rounds), static_cast<float>(k_rounds));
    zassert_equal(b_recorder->Count(), 1);
    zassert_equal(b_recorder->At(0), 100.0F);
}

namespace {

// Hands one reading to the pipeline on the work queue, as an edge source does, and waits for it.
// A plain work item: WorkQueueThread::Run() allocates.
struct Feed {
    k_work work;
    k_sem done;
    ReadingPipeline* pipeline;
    const SensorRuntime* runtime;
    float value;

    static void Handler(k_work* work) {
        auto* feed = CONTAINER_OF(work, Feed, work);

        SensorReading reading(feed->runtime->sensor.get());
        reading.source = ReadingSource::ISR;
        reading.status = ReadingStatus::RAW;
        reading.value = feed->value;
        reading.raw_value = feed->value;

        feed->pipeline->Process(*feed->runtime, reading);
        k_sem_give(&feed->done);
    }

    void Run(k_work_q* work_queue, float input) {
        value = input;
        zassert_true(k_work_submit_to_queue(work_queue, &work) >= 0);
        zassert_ok(k_sem_take(&done, K_SECONDS(1)));
    }
};

} // namespace

// Every input change is held, then committed by the conditioner's work item with the switch and
// the dependent evaluated. Only the counts are asserted off native_sim, whose simulated clock
// makes the values exact.
ZTEST(allocation_free_pipeline, test_conditioned_indicators_do_not_allocate) {
    constexpr int kDebounceMs = 20;
    constexpr int kSettleMs = 50;
    constexpr size_t k_rounds = 10;

    auto work_queue_thread = std::make_shared<WorkQueueThread>(
        "conditioning_alloc",
        CONFIG_EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_SERVICE_STACK_SIZE,
        CONFIG_EERIE_LEAP_DOMAIN_SENSOR_PROCESSING_SERVICE_PRIORITY);
    zassert_true(work_queue_thread->Initialize());

    auto time_service = std::make_shared<TimeService>(
        std::make_shared<BootElapsedTimeProvider>(), std::make_shared<RtcProvider>());
    auto frame = std::make_shared<SensorReadingsFrame>();

    auto button = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "cond_alloc_button");
    button->configuration.type = SensorType::PHYSICAL_INDICATOR;
    button->configuration.channel = 0;
    button->configuration.debounce_ms = kDebounceMs;
    button->configuration.input_mode = IndicatorInputMode::Toggle;

    auto probe = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "cond_alloc_probe");
    probe->configuration.type = SensorType::VIRTUAL_ANALOG;
    probe->configuration.expression = "cond_alloc_button";

    const std::vector<std::shared_ptr<Sensor>> sensors { button, probe };
    auto generation = SensorPipelineBuilder(nullptr, frame).Build(
        std::make_shared<const std::vector<std::shared_ptr<Sensor>>>(sensors));
    zassert_not_null(generation->runtimes[0].conditioner.get());

    auto processors = std::make_shared<std::vector<std::shared_ptr<IReadingProcessor>>>();
    processors->push_back(std::make_shared<ExpressionProcessor>(frame));
    processors->push_back(std::make_shared<ScriptProcessor>("post_process_sensor_value"));
    auto pipeline = std::make_shared<ReadingPipeline>(frame, processors, time_service, work_queue_thread);

    Feed feed{};
    k_work_init(&feed.work, Feed::Handler);
    k_sem_init(&feed.done, 0, 1);
    feed.pipeline = pipeline.get();
    feed.runtime = &generation->runtimes[0];

    k_work_q* work_queue = work_queue_thread->GetWorkQueue();

    // Outside the window: the first value, then one press settled by the timer.
    feed.Run(work_queue, 0.0F);
    feed.Run(work_queue, 1.0F);
    k_msleep(kSettleMs);
    feed.Run(work_queue, 0.0F);
    k_msleep(kSettleMs);

    std::array<float, k_rounds> held {};
    std::array<float, k_rounds> pressed {};
    std::array<float, k_rounds> released {};

    const size_t allocations_before = GetAllocationCount();

    for(size_t i = 0; i < k_rounds; i++) {
        feed.Run(work_queue, 1.0F);
        held[i] = frame->TryGetReadingValue("cond_alloc_button").value_or(-1.0F);
        k_msleep(kSettleMs);
        pressed[i] = frame->TryGetReadingValue("cond_alloc_probe").value_or(-1.0F);

        feed.Run(work_queue, 0.0F);
        k_msleep(kSettleMs);
        released[i] = frame->TryGetReadingValue("cond_alloc_probe").value_or(-1.0F);
    }

    const size_t allocations = GetAllocationCount() - allocations_before;

    // Nothing may settle into the generation once it goes.
    generation->CancelSettles();

    zassert_equal(allocations, 0, "%zu allocations during %zu conditioned presses", allocations, k_rounds);

    if(IS_ENABLED(CONFIG_BOARD_NATIVE_SIM)) {
        // The switch was on after the warm-up press, so the presses turn it off, on, off, ...
        for(size_t i = 0; i < k_rounds; i++) {
            const float before = (i % 2 == 0) ? 1.0F : 0.0F;
            const float after = 1.0F - before;

            zassert_equal(held[i], before, "Round %zu: the press is held", i);
            zassert_equal(pressed[i], after, "Round %zu: the timer settled the press, the dependent followed", i);
            zassert_equal(released[i], after, "Round %zu: the release leaves the switch", i);
        }
    }
}
