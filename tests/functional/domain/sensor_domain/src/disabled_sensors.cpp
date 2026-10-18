// A disabled sensor keeps its slot, so lookups by its ID still resolve, but nothing reads it: it has
// no update method and nothing executable, never gets a reading, and the other sensors run as usual.
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <eerie_memory.hpp>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "domain/sensor_domain/models/reading_source.h"
#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/sensor_domain/models/sensor_reading_update_method.h"
#include "domain/sensor_domain/processors/expression_processor.h"
#include "domain/sensor_domain/processors/reading_pipeline.hpp"
#include "domain/sensor_domain/runtime/sensor_pipeline_builder.h"
#include "utilities/string/string_helpers.h"

#include "processing_test_support.h"

using namespace eerie_leap::utilities::string;
using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::sensor_domain::runtime;
using namespace eerie_leap::domain::sensor_domain::processors;

using namespace processing_test_support;

ZTEST_SUITE(disabled_sensors, NULL, NULL, NULL, NULL, NULL);

namespace {

std::shared_ptr<Sensor> Disabled(std::shared_ptr<Sensor> sensor) {
    sensor->is_enabled = false;

    return sensor;
}

// Without a sampling rate, so it is evaluated when its input commits.
std::shared_ptr<Sensor> MakeDependentSensor(std::string_view id, const char* expression) {
    auto sensor = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), id);
    sensor->configuration.type = SensorType::VIRTUAL_ANALOG;
    sensor->configuration.expression = expression;

    return sensor;
}

struct Fixture {
    std::shared_ptr<SensorReadingsFrame> frame;
    std::shared_ptr<SensorGeneration> generation;

    const SensorRuntime& Runtime(std::string_view id) const {
        return *generation->Find(StringHelpers::GetHash(id));
    }
};

// Enabled, they would get an interpolator, GPIO edges, or an expression bound to input.
constexpr std::array<std::string_view, 4> DISABLED_IDS = { "analog", "indicator", "disabled_dependent",
    "disabled_polled" };

// input (polled) -> dependent, next to the disabled sensors.
Fixture MakeFixture() {
    const auto indicator = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "indicator");
    indicator->configuration.type = SensorType::PHYSICAL_INDICATOR;
    indicator->configuration.channel = 0;

    const std::vector<std::shared_ptr<Sensor>> sensors = {
        Disabled(MakeAnalogSensor("analog", 0, 20)),
        Disabled(indicator),
        MakeVirtualSensor("input", "1 + 1", 1000),
        MakeDependentSensor("dependent", "input * 2"),
        Disabled(MakeDependentSensor("disabled_dependent", "input * 3")),
        Disabled(MakeVirtualSensor("disabled_polled", "input + 1", 1000)),
    };

    Fixture fixture;
    fixture.frame = std::make_shared<SensorReadingsFrame>();
    fixture.generation = SensorPipelineBuilder(nullptr, fixture.frame)
                             .Build(std::make_shared<const std::vector<std::shared_ptr<Sensor>>>(sensors));

    return fixture;
}

// Whether the sensor's reading changes its timestamp @p count times within a second.
bool KeepsUpdating(const SensorReadingsFrame& frame, std::string_view id, int count) {
    std::optional<std::chrono::system_clock::time_point> last;
    int updates = 0;

    const int64_t end = k_uptime_get() + 1000;
    while(updates < count && k_uptime_get() < end) {
        const auto reading = frame.TryGetReading(id);
        if(reading.has_value() && reading->timestamp.has_value() && reading->timestamp != last) {
            last = reading->timestamp;
            updates++;
        }

        k_msleep(5);
    }

    return updates >= count;
}

} // namespace

ZTEST(disabled_sensors, test_builder_keeps_the_slot_of_a_disabled_sensor_and_builds_nothing_for_it) {
    const auto fixture = MakeFixture();

    zassert_equal(fixture.generation->runtimes.size(), 6, "Every sensor has a runtime");
    zassert_equal(fixture.frame->GetSlotCount(), 6, "Every sensor has a slot");

    for(const auto id : DISABLED_IDS) {
        const auto& runtime = fixture.Runtime(id);
        const auto slot = fixture.frame->FindSlot(id);

        zassert_true(
            slot.has_value() && runtime.slot == slot.value(), "%.*s lost its slot", static_cast<int>(id.size()),
            id.data()
        );
        zassert_equal(
            runtime.update_method, SensorReadingUpdateMethod::NONE, "%.*s has an update method",
            static_cast<int>(id.size()), id.data()
        );
        zassert_is_null(
            runtime.voltage_interpolator.get(), "%.*s has an interpolator", static_cast<int>(id.size()), id.data()
        );
        zassert_is_null(
            runtime.expression_evaluator.get(), "%.*s has an expression evaluator", static_cast<int>(id.size()),
            id.data()
        );
        zassert_true(
            runtime.input_slots.empty(), "%.*s is bound to its inputs", static_cast<int>(id.size()), id.data()
        );
        zassert_true(runtime.dependents.empty(), "%.*s has dependents", static_cast<int>(id.size()), id.data());
    }

    const auto& input = fixture.Runtime("input");
    const auto& dependent = fixture.Runtime("dependent");
    zassert_equal(input.update_method, SensorReadingUpdateMethod::SCHEDULER);
    zassert_not_null(input.expression_evaluator.get());
    zassert_equal(dependent.update_method, SensorReadingUpdateMethod::DEPENDENT);

    zassert_equal(input.dependents.size(), 1, "Only the enabled dependent is linked to the input");
    zassert_equal(input.dependents[0], &dependent);
}

ZTEST(disabled_sensors, test_a_disabled_dependent_is_not_evaluated_when_its_input_commits) {
    const auto fixture = MakeFixture();
    const auto time_service = MakeTimeService();

    const auto processors = std::make_shared<std::vector<std::shared_ptr<IReadingProcessor>>>();
    processors->push_back(std::make_shared<ExpressionProcessor>(fixture.frame));
    ReadingPipeline pipeline(fixture.frame, processors, time_service);

    const auto& input = fixture.Runtime("input");
    SensorReading reading(input.sensor.get());
    reading.source = ReadingSource::PROCESSING;
    reading.timestamp = time_service->GetCurrentTime();
    pipeline.Process(input, reading);

    zassert_equal(fixture.frame->TryGetReadingValue("input").value(), 2.0F);
    zassert_equal(fixture.frame->TryGetReadingValue("dependent").value(), 4.0F, "The enabled dependent is evaluated");

    for(const auto id : DISABLED_IDS) {
        zassert_false(
            fixture.frame->TryGetReading(id).has_value(), "%.*s got a reading", static_cast<int>(id.size()), id.data()
        );
        zassert_false(
            fixture.frame->TryGetReadingValue(id).has_value(), "%.*s got a value", static_cast<int>(id.size()),
            id.data()
        );
    }
}

ZTEST(disabled_sensors, test_a_disabled_sensor_never_gets_a_reading_while_the_others_run) {
    const auto fs_service = MakeFormattedFsService();

    const auto adc = std::make_shared<FakeAdc>(4);
    const auto adc_manager = MakeAdcManager(adc);
    const auto adc_configuration_manager = MakeAdcConfigurationManager(fs_service, adc_manager);
    zassert_true(adc_configuration_manager->Update(
        *MakeAdcConfiguration(adc_manager->GetChannelCount(), 8, MakeCalibrator({ { 0.0F, 0.0F }, { 5.0F, 5.0F } }))
    ));

    // A disabled sensor may read an enabled one.
    const std::vector<std::shared_ptr<Sensor>> disabled = {
        Disabled(MakeAnalogSensor("disabled_analog", 0, 20)),
        Disabled(MakeVirtualSensor("disabled_virtual", "enabled_virtual + 1", 20)),
    };

    const auto rig = MakeProcessingRig(
        fs_service, adc_manager, adc_configuration_manager,
        {
            disabled[0],
            MakeAnalogSensor("enabled_analog", 1, 20),
            MakeVirtualSensor("enabled_virtual", "40 + 2", 20),
            disabled[1],
        },
        adc_manager->GetChannelCount()
    );

    const int requests_before = adc->requests;

    zassert_true(rig.processing_service->Start());

    zassert_true(KeepsUpdating(*rig.frame, "enabled_virtual", 3), "The enabled virtual sensor stopped updating");
    zassert_true(KeepsUpdating(*rig.frame, "enabled_analog", 3), "The enabled analog sensor stopped updating");
    zassert_within(rig.frame->TryGetReadingValue("enabled_virtual").value(), 42.0F, 1e-3F);

    for(const auto& sensor : disabled) {
        const char* id = sensor->id.c_str();

        zassert_true(rig.frame->FindSlot(sensor->id_hash).has_value(), "%s lost its slot", id);
        zassert_false(rig.frame->HasReading(sensor->id_hash), "%s got a reading", id);

        // What a binding or a script reads for it.
        const float* value = rig.frame->GetReadingValuePtr(sensor->id);
        zassert_not_null(value, "%s cannot be looked up", id);
        zassert_true(std::isnan(*value), "%s has a value", id);
    }

    zassert_true(rig.processing_service->Stop());

    zassert_true(adc->requests > requests_before, "The enabled analog sensor was not sampled");
    zassert_equal(adc->last_channel_count, 1, "The disabled analog sensor's channel was requested");
    zassert_equal(adc->last_channels[0], 1);
}
