// Guards the rule the sensor pipeline is built on: allocate when the configuration changes,
// never when a sample arrives. Counts every heap allocation in the process while a sample goes
// from a reader through the processors into the frame and out to a consumer.

#include <array>
#include <cstdlib>
#include <memory>
#include <new>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"
#include "subsys/time/rtc_provider.h"
#include "subsys/time/boot_elapsed_time_provider.h"
#include "subsys/time/time_service.h"
#include "subsys/gpio/gpio_simulator.h"
#include "subsys/canbus/can_frame.h"
#include "subsys/expression_engine/expression_evaluator.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "domain/sensor_domain/sensor_readers/sensor_reader_physical_indicator.h"
#include "domain/sensor_domain/sensor_readers/sensor_reader_virtual_analog.h"
#include "domain/sensor_domain/processors/expression_processor.h"
#include "domain/sensor_domain/processors/script_processor.h"
#include "domain/sensor_domain/processors/reading_pipeline.hpp"
#include "domain/sensor_domain/runtime/sensor_pipeline_builder.h"

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::subsys::time;
using namespace eerie_leap::subsys::gpio;
using namespace eerie_leap::subsys::canbus;
using namespace eerie_leap::subsys::expression_engine;
using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::sensor_domain::utilities;
using namespace eerie_leap::domain::sensor_domain::sensor_readers;
using namespace eerie_leap::domain::sensor_domain::processors;
using namespace eerie_leap::domain::sensor_domain::runtime;

ZTEST_SUITE(allocation_free_pipeline, NULL, NULL, NULL, NULL, NULL);

// Replaces the global allocation functions for this test binary so every allocation is counted.
namespace {

size_t g_allocations = 0;

void* CountedAllocate(size_t size) {
    g_allocations++;

    void* pointer = std::malloc(size == 0 ? 1 : size);
    if(pointer == nullptr)
        throw std::bad_alloc();

    return pointer;
}

} // namespace

void* operator new(size_t size) { return CountedAllocate(size); }
void* operator new[](size_t size) { return CountedAllocate(size); }
void* operator new(size_t size, const std::nothrow_t&) noexcept { g_allocations++; return std::malloc(size == 0 ? 1 : size); }
void* operator new[](size_t size, const std::nothrow_t&) noexcept { g_allocations++; return std::malloc(size == 0 ? 1 : size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, size_t) noexcept { std::free(pointer); }

namespace {

struct Pipeline {
    std::shared_ptr<SensorReadingsFrame> frame;
    std::shared_ptr<GpioSimulator> gpio;
    std::vector<std::shared_ptr<Sensor>> sensors;          // indicator, virtual, raw CAN
    std::shared_ptr<SensorGeneration> generation;
    std::vector<std::unique_ptr<ISensorReader>> readers;   // indicator, virtual
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

    pipeline.sensors = { indicator, derived, raw, dependent };
    // Lays out the slots and binds "indicator" in the derived expression to its value slot.
    pipeline.generation = SensorPipelineBuilder(nullptr, pipeline.frame).Build(
        std::make_shared<const std::vector<std::shared_ptr<Sensor>>>(pipeline.sensors));
    pipeline.readers.push_back(std::make_unique<SensorReaderPhysicalIndicator>(time_service, pipeline.generation->runtimes[0], pipeline.gpio));
    pipeline.readers.push_back(std::make_unique<SensorReaderVirtualAnalog>(time_service, pipeline.generation->runtimes[1]));
    pipeline.processors = std::make_shared<std::vector<std::shared_ptr<IReadingProcessor>>>();
    pipeline.processors->push_back(std::make_shared<ExpressionProcessor>(pipeline.frame));
    pipeline.processors->push_back(std::make_shared<ScriptProcessor>("post_process_sensor_value"));
    pipeline.pipeline = std::make_shared<ReadingPipeline>(pipeline.frame, pipeline.processors, time_service);

    return pipeline;
}

// One sample of every source kind, the way the services run them.
void RunSamples(Pipeline& pipeline, uint32_t iteration) {
    // Readers in generation order; committing the indicator also evaluates the dependent sensor.
    for(size_t i = 0; i < pipeline.readers.size(); i++) {
        SensorReading reading = pipeline.readers[i]->Read();
        pipeline.pipeline->Process(pipeline.generation->runtimes[i], reading);
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
    const size_t before = g_allocations;
    auto block = std::make_unique<uint8_t[]>(32);

    zassert_equal(g_allocations - before, 1, "The replaced operator new must count, or the test below proves nothing");
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

    const size_t allocations_before = g_allocations;

    for(uint32_t i = 3; i < 103; i++) {
        RunSamples(pipeline, i);
        RunConsumers(pipeline);
    }

    zassert_equal(g_allocations - allocations_before, 0,
        "%zu allocations during 100 steady-state samples", g_allocations - allocations_before);

    // And the pipeline still produced the right results while being counted (the simulator's level is random).
    const float indicator = pipeline.frame->TryGetReadingValue("indicator").value();
    zassert_true(indicator == 0.0F || indicator == 1.0F);
    zassert_equal(pipeline.frame->TryGetReadingValue("derived").value(), indicator * 2.0F + 1.0F);
    zassert_equal(pipeline.frame->TryGetReadingValue("dependent").value(), indicator + indicator * 2.0F + 1.0F,
        "The dependent sensor was evaluated after the indicator, with the derived value of this pass");
    zassert_equal(pipeline.frame->GetDroppedRawCanFrameCount(), 0, "The log writer path drained the raw queue every sample");

    CanFrame can_frame;
    zassert_true(pipeline.frame->TryGetCanFrame(pipeline.sensors[2]->id_hash, can_frame));
    zassert_equal(can_frame.id, 0x100 + 102);
}
