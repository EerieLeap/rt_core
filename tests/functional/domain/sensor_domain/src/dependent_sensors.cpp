// Virtual sensors without a sampling rate are evaluated right after one of their inputs commits.

#include <memory>
#include <vector>

#include <zephyr/ztest.h>
#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"
#include "subsys/time/rtc_provider.h"
#include "subsys/time/boot_elapsed_time_provider.h"
#include "subsys/time/time_service.h"
#include "subsys/gpio/gpio_simulator.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "domain/sensor_domain/runtime/sensor_pipeline_builder.h"
#include "domain/sensor_domain/sensor_readers/sensor_reader_physical_indicator.h"
#include "domain/sensor_domain/processors/expression_processor.h"
#include "domain/sensor_domain/processors/reading_pipeline.hpp"

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::subsys::time;
using namespace eerie_leap::subsys::gpio;
using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::sensor_domain::utilities;
using namespace eerie_leap::domain::sensor_domain::runtime;
using namespace eerie_leap::domain::sensor_domain::sensor_readers;
using namespace eerie_leap::domain::sensor_domain::processors;

ZTEST_SUITE(dependent_sensors, NULL, NULL, NULL, NULL, NULL);

namespace {

struct Fixture {
    std::shared_ptr<SensorReadingsFrame> frame;
    std::shared_ptr<SensorGeneration> generation;
    std::shared_ptr<ReadingPipeline> pipeline;
    std::unique_ptr<ISensorReader> indicator_reader;

    const SensorRuntime& Runtime(std::string_view id) const {
        return *generation->Find(StringHelpers::GetHash(id));
    }
};

std::shared_ptr<Sensor> MakeVirtual(std::string_view id, const char* expression, std::optional<int> sampling_rate_ms = std::nullopt) {
    auto sensor = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), id);
    sensor->configuration.type = SensorType::VIRTUAL_ANALOG;
    sensor->configuration.expression = expression;
    sensor->configuration.sampling_rate_ms = sampling_rate_ms;

    return sensor;
}

// indicator (GPIO) -> doubled (dependent) -> doubled_plus (dependent of a dependent);
// polled (virtual with a sampling rate) also reads the indicator but keeps its timer.
Fixture MakeFixture() {
    Fixture fixture;

    auto time_service = std::make_shared<TimeService>(
        std::make_shared<BootElapsedTimeProvider>(), std::make_shared<RtcProvider>());

    auto gpio = std::make_shared<GpioSimulator>();
    gpio->Initialize();

    auto indicator = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "indicator");
    indicator->configuration.type = SensorType::PHYSICAL_INDICATOR;
    indicator->configuration.channel = 0;

    std::vector<std::shared_ptr<Sensor>> sensors = {
        indicator,
        MakeVirtual("doubled", "indicator * 2"),
        MakeVirtual("doubled_plus", "doubled + 1"),
        MakeVirtual("polled", "indicator + 100", 1000),
    };

    fixture.frame = std::make_shared<SensorReadingsFrame>();
    fixture.generation = SensorPipelineBuilder(nullptr, fixture.frame).Build(
        std::make_shared<const std::vector<std::shared_ptr<Sensor>>>(sensors));

    auto processors = std::make_shared<std::vector<std::shared_ptr<IReadingProcessor>>>();
    processors->push_back(std::make_shared<ExpressionProcessor>(fixture.frame));
    fixture.pipeline = std::make_shared<ReadingPipeline>(fixture.frame, processors, time_service);

    fixture.indicator_reader = std::make_unique<SensorReaderPhysicalIndicator>(time_service, fixture.Runtime("indicator"), gpio);

    return fixture;
}

} // namespace

ZTEST(dependent_sensors, test_builder_links_dependents_transitively_in_processing_order) {
    auto fixture = MakeFixture();

    const auto& indicator = fixture.Runtime("indicator");
    const auto& doubled = fixture.Runtime("doubled");
    const auto& doubled_plus = fixture.Runtime("doubled_plus");
    const auto& polled = fixture.Runtime("polled");

    zassert_equal(indicator.update_method, SensorReadingUpdateMethod::ISR);
    zassert_equal(doubled.update_method, SensorReadingUpdateMethod::DEPENDENT);
    zassert_equal(doubled_plus.update_method, SensorReadingUpdateMethod::DEPENDENT);
    zassert_equal(polled.update_method, SensorReadingUpdateMethod::SCHEDULER, "A sampling rate keeps the timer");

    zassert_equal(indicator.dependents.size(), 2, "Direct and transitive dependents, not the polled one");
    zassert_equal(indicator.dependents[0], &doubled);
    zassert_equal(indicator.dependents[1], &doubled_plus);

    zassert_equal(doubled.dependents.size(), 1);
    zassert_equal(doubled.dependents[0], &doubled_plus);
    zassert_equal(doubled_plus.dependents.size(), 0);
    zassert_equal(polled.dependents.size(), 0);
}

ZTEST(dependent_sensors, test_committing_an_input_evaluates_its_dependents) {
    auto fixture = MakeFixture();

    zassert_false(fixture.frame->TryGetReadingValue("doubled").has_value());

    SensorReading reading = fixture.indicator_reader->Read();
    fixture.pipeline->Process(fixture.Runtime("indicator"), reading);

    const float indicator = fixture.frame->TryGetReadingValue("indicator").value();
    zassert_true(indicator == 0.0F || indicator == 1.0F);

    const auto doubled = fixture.frame->TryGetReading("doubled").value();
    zassert_equal(doubled.status, ReadingStatus::PROCESSED);
    zassert_equal(doubled.source, ReadingSource::PROCESSING);
    zassert_true(doubled.timestamp.has_value());
    zassert_equal(doubled.value.value(), indicator * 2.0F);

    zassert_equal(fixture.frame->TryGetReadingValue("doubled_plus").value(), indicator * 2.0F + 1.0F,
        "A dependent of a dependent sees the fresh value in the same pass");

    zassert_false(fixture.frame->TryGetReadingValue("polled").has_value(), "A polled virtual sensor waits for its timer");
}

ZTEST(dependent_sensors, test_a_failed_input_leaves_its_dependents_untouched) {
    auto fixture = MakeFixture();

    SensorReading reading = fixture.indicator_reader->Read();
    fixture.pipeline->Process(fixture.Runtime("indicator"), reading);
    const float before = fixture.frame->TryGetReadingValue("doubled").value();

    SensorReading failed(fixture.Runtime("indicator").sensor.get());
    failed.source = ReadingSource::PROCESSING;
    failed.SetError(ReadingError::READER_FAILED);
    fixture.pipeline->Process(fixture.Runtime("indicator"), failed);

    zassert_equal(fixture.frame->TryGetReading("indicator").value().status, ReadingStatus::ERROR);
    zassert_equal(fixture.frame->TryGetReadingValue("doubled").value(), before);
}
