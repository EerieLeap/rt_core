// A blank USER sensor, which nothing of its own gives a value, holds what scripts write with
// update_sensor_value(): its ticks carry that value, so the log and its dependents follow it.
#include <array>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <eerie_memory.hpp>
#include <zephyr/ztest.h>

#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/sensor_domain/processors/expression_processor.h"
#include "domain/sensor_domain/processors/reading_pipeline.hpp"
#include "domain/sensor_domain/processors/script_processor.h"
#include "domain/sensor_domain/runtime/sensor_pipeline_builder.h"
#include "domain/sensor_domain/sensor_readers/sensor_reader_user_value_type.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "subsys/time/boot_elapsed_time_provider.h"
#include "subsys/time/rtc_provider.h"
#include "subsys/time/time_service.h"
#include "utilities/memory/memory_resource_manager.h"
#include "utilities/string/string_helpers.h"

using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::subsys::time;
using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::sensor_domain::utilities;
using namespace eerie_leap::domain::sensor_domain::runtime;
using namespace eerie_leap::domain::sensor_domain::sensor_readers;
using namespace eerie_leap::domain::sensor_domain::processors;
using eerie_leap::utilities::string::StringHelpers;

ZTEST_SUITE(blank_user_sensors, NULL, NULL, NULL, NULL, NULL);

namespace {

struct Fixture {
    std::shared_ptr<TimeService> time_service;
    std::shared_ptr<SensorReadingsFrame> frame;
    std::shared_ptr<SensorGeneration> generation;
    std::shared_ptr<ReadingPipeline> pipeline;

    const SensorRuntime& Runtime(std::string_view id) const {
        return *generation->Find(StringHelpers::GetHash(id));
    }

    // One tick of the sensor's rate, as RateGroupScheduler runs it.
    void Tick(std::string_view id) {
        const auto& runtime = Runtime(id);
        SensorReaderUserValueType reader(time_service, runtime);

        auto reading = reader.Read({ time_service->GetCurrentTime() });
        pipeline->Process(runtime, reading);
    }

    // What update_sensor_value(id, value) does.
    bool Write(std::string_view id, float value) {
        return frame->UpdateReadingValue(StringHelpers::GetHash(id), value);
    }

    // The sensor's reading as the log writer samples it.
    std::optional<SensorReading> Logged(std::string_view id) const {
        std::array<SensorReading, 8> readings;
        const size_t count = frame->SnapshotLoggableReadings(readings);

        for(size_t i = 0; i < count; i++) {
            if(readings[i].sensor_id_hash == StringHelpers::GetHash(id))
                return readings[i];
        }

        return std::nullopt;
    }
};

std::shared_ptr<Sensor> MakeSensor(std::string_view id, SensorType type) {
    auto sensor = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), id);
    sensor->configuration.type = type;
    sensor->configuration.sampling_rate_ms = 100;

    return sensor;
}

// blank (no script, no expression) -> follower (dependent); with_expression is not blank;
// blank_switch is a blank Toggle indicator, so it has a conditioner.
Fixture MakeFixture() {
    Fixture fixture;

    fixture.time_service = std::make_shared<TimeService>(
        std::make_shared<BootElapsedTimeProvider>(), std::make_shared<RtcProvider>()
    );

    const auto with_expression = MakeSensor("with_expression", SensorType::USER_ANALOG);
    with_expression->configuration.expression = "blank * 2";

    const auto follower = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), "follower");
    follower->configuration.type = SensorType::VIRTUAL_ANALOG;
    follower->configuration.expression = "blank + 1";

    const auto blank_switch = MakeSensor("blank_switch", SensorType::USER_INDICATOR);
    blank_switch->configuration.input_mode = IndicatorInputMode::Toggle;

    const std::vector<std::shared_ptr<Sensor>> sensors = {
        MakeSensor("blank", SensorType::USER_ANALOG),
        with_expression,
        follower,
        blank_switch,
    };

    fixture.frame = std::make_shared<SensorReadingsFrame>();
    fixture.generation = SensorPipelineBuilder(nullptr, fixture.frame)
                             .Build(std::make_shared<const std::vector<std::shared_ptr<Sensor>>>(sensors));

    const auto processors = std::make_shared<std::vector<std::shared_ptr<IReadingProcessor>>>();
    processors->push_back(std::make_shared<ExpressionProcessor>(fixture.frame));
    processors->push_back(std::make_shared<ScriptProcessor>(SensorScript::POST_PROCESS_FUNCTION));
    fixture.pipeline = std::make_shared<ReadingPipeline>(fixture.frame, processors, fixture.time_service);

    return fixture;
}

} // namespace

ZTEST(blank_user_sensors, test_only_a_sensor_without_script_functions_or_expression_is_blank) {
    const auto fixture = MakeFixture();

    zassert_not_null(fixture.Runtime("blank").written_value);
    zassert_not_null(fixture.Runtime("blank_switch").written_value);
    zassert_is_null(fixture.Runtime("with_expression").written_value, "Its expression gives it a value");
    zassert_is_null(fixture.Runtime("follower").written_value, "Not a USER sensor");
}

ZTEST(blank_user_sensors, test_a_tick_carries_the_value_last_written) {
    auto fixture = MakeFixture();

    // Before anything is written the tick commits a reading without a value, which lets a script
    // write the sensor.
    zassert_false(fixture.Write("blank", 1.0F), "No reading yet");
    fixture.Tick("blank");
    zassert_false(fixture.frame->TryGetReadingValue("blank").has_value());
    zassert_true(fixture.Logged("blank").has_value());
    zassert_false(fixture.Logged("blank")->value.has_value());

    zassert_true(fixture.Write("blank", 7.0F));
    zassert_equal(fixture.Logged("blank")->value.value(), 7.0F);

    // The ticks after the write keep it, instead of replacing it with a reading without a value.
    for(int i = 0; i < 3; i++) {
        fixture.Tick("blank");

        const auto logged = fixture.Logged("blank");
        zassert_true(logged.has_value());
        zassert_equal(logged->status, ReadingStatus::PROCESSED);
        zassert_equal(logged->value.value(), 7.0F, "Tick %d logged the value written", i);
        zassert_equal(fixture.frame->TryGetReadingValue("blank").value(), 7.0F);
    }

    zassert_equal(fixture.frame->TryGetReadingValue("follower").value(), 8.0F, "The tick evaluated the dependent");

    zassert_true(fixture.Write("blank", -2.5F));
    fixture.Tick("blank");
    zassert_equal(fixture.Logged("blank")->value.value(), -2.5F);
    zassert_equal(fixture.frame->TryGetReadingValue("follower").value(), -1.5F);
}

// What a script wrote already went past the conditioner: a Toggle would flip on the carried 1
// and hold on the carried 0.
ZTEST(blank_user_sensors, test_a_carried_value_bypasses_the_conditioner) {
    auto fixture = MakeFixture();
    zassert_not_null(fixture.Runtime("blank_switch").conditioner.get());

    fixture.pipeline->CommitStartValues(*fixture.generation);
    zassert_equal(fixture.frame->TryGetReadingValue("blank_switch").value(), 0.0F, "The switch starts off");

    zassert_true(fixture.Write("blank_switch", 1.0F));
    fixture.Tick("blank_switch");
    zassert_equal(fixture.Logged("blank_switch")->value.value(), 1.0F);

    zassert_true(fixture.Write("blank_switch", 0.0F));
    fixture.Tick("blank_switch");
    zassert_equal(fixture.Logged("blank_switch")->value.value(), 0.0F);
}
