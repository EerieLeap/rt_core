// Scripted sensors share one Lua state per generation: each script runs in its own environment,
// its entry points are resolved once, and the state's memory is capped.
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>
#include <zephyr/ztest.h>
#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"
#include "subsys/time/time_service.h"
#include "subsys/time/rtc_provider.h"
#include "subsys/time/boot_elapsed_time_provider.h"
#include "subsys/lua_script/lua_script.h"
#include "domain/script_domain/utilities/global_fuctions_registry.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "domain/sensor_domain/runtime/sensor_runtime.h"
#include "domain/sensor_domain/sensor_readers/sensor_reader_user_value_type.h"
#include "domain/sensor_domain/processors/script_processor.h"

using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::subsys::time;
using namespace eerie_leap::subsys::lua_script;
using namespace eerie_leap::domain::script_domain::utilities;
using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::sensor_domain::utilities;
using namespace eerie_leap::domain::sensor_domain::runtime;
using namespace eerie_leap::domain::sensor_domain::sensor_readers;
using namespace eerie_leap::domain::sensor_domain::processors;

ZTEST_SUITE(lua_scripts, NULL, NULL, NULL, NULL, NULL);

namespace {

std::span<const uint8_t> Bytes(std::string_view text) {
    return { reinterpret_cast<const uint8_t*>(text.data()), text.size() };
}

std::shared_ptr<Sensor> MakeUserSensor(std::string_view id) {
    auto sensor = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), id);
    sensor->configuration.type = SensorType::USER_ANALOG;
    sensor->configuration.sampling_rate_ms = 100;

    return sensor;
}

// A runtime whose script is loaded into @p host, the way SensorPipelineBuilder does it.
SensorRuntime MakeScriptedRuntime(std::shared_ptr<LuaScript> host, std::string_view id, std::string_view script) {
    SensorRuntime runtime;
    runtime.sensor = MakeUserSensor(id);
    runtime.script.host = host;

    auto guard = host->Lock();
    runtime.script.environment = host->LoadChunk(Bytes(script), runtime.sensor->id.c_str());
    runtime.script.create_value = host->FindFunction(runtime.script.environment, SensorScript::kCreateValueFunction);
    runtime.script.post_process = host->FindFunction(runtime.script.environment, SensorScript::kPostProcessFunction);

    return runtime;
}

std::shared_ptr<TimeService> MakeTimeService() {
    return std::make_shared<TimeService>(std::make_shared<BootElapsedTimeProvider>(), std::make_shared<RtcProvider>());
}

SensorReading MakeReading(const SensorRuntime& runtime, float value) {
    SensorReading reading(runtime.sensor.get());
    reading.source = ReadingSource::PROCESSING;
    reading.value = value;
    reading.status = ReadingStatus::RAW;

    return reading;
}

} // namespace

ZTEST(lua_scripts, test_scripts_in_one_state_do_not_share_globals) {
    auto host = std::make_shared<LuaScript>(LuaScript::CreateExt());

    // Both scripts define "scale" and "create_sensor_value"; each must see its own.
    auto runtime_a = MakeScriptedRuntime(host, "sensor_a",
        "scale = 2\nfunction create_sensor_value(id) return scale * 10 end\n");
    auto runtime_b = MakeScriptedRuntime(host, "sensor_b",
        "scale = 5\nfunction create_sensor_value(id) return scale * 10 end\n");

    zassert_true(runtime_a.script.HasCreateValue());
    zassert_true(runtime_b.script.HasCreateValue());
    zassert_false(runtime_a.script.HasPostProcess(), "Not defined by this script");

    auto time_service = MakeTimeService();
    SensorReaderUserValueType reader_a(time_service, runtime_a);
    SensorReaderUserValueType reader_b(time_service, runtime_b);

    zassert_equal(reader_a.Read().value.value(), 20.0F);
    zassert_equal(reader_b.Read().value.value(), 50.0F);
    zassert_equal(reader_a.Read().value.value(), 20.0F, "Running b did not change a's globals");

    // The standard libraries and the host functions stay visible through the environment.
    auto runtime_c = MakeScriptedRuntime(host, "sensor_c",
        "function create_sensor_value(id) return math.floor(string.len(id) * 1.5) end\n");
    SensorReaderUserValueType reader_c(time_service, runtime_c);
    zassert_equal(reader_c.Read().value.value(), 12.0F);
}

ZTEST(lua_scripts, test_a_script_that_fails_to_load_has_no_environment) {
    auto host = std::make_shared<LuaScript>(LuaScript::CreateExt());

    auto broken = MakeScriptedRuntime(host, "broken", "function (\n");
    zassert_false(broken.script.IsLoaded());
    zassert_false(broken.script.HasCreateValue());

    auto runtime_error = MakeScriptedRuntime(host, "runtime_error", "error('boom')\n");
    zassert_false(runtime_error.script.IsLoaded());

    // The state keeps serving the scripts that did load.
    auto fine = MakeScriptedRuntime(host, "fine", "function create_sensor_value(id) return 1 end\n");
    zassert_true(fine.script.HasCreateValue());

    SensorReaderUserValueType reader(MakeTimeService(), broken);
    zassert_equal(reader.Read().status, ReadingStatus::UNINITIALIZED, "No function, no value");
}

ZTEST(lua_scripts, test_post_process_receives_the_sample_and_returns_the_new_value) {
    auto host = std::make_shared<LuaScript>(LuaScript::CreateExt());

    auto doubling = MakeScriptedRuntime(host, "doubling",
        "function post_process_sensor_value(id, value) return value * 2 end\n");
    auto keeping = MakeScriptedRuntime(host, "keeping",
        "seen = nil\nfunction post_process_sensor_value(id, value) seen = value return nil end\n");
    auto failing = MakeScriptedRuntime(host, "failing",
        "function post_process_sensor_value(id, value) error('no') end\n");

    ScriptProcessor processor(SensorScript::kPostProcessFunction);

    auto reading = MakeReading(doubling, 21.0F);
    processor.Process(doubling, reading);
    zassert_false(reading.HasError());
    zassert_equal(reading.value.value(), 42.0F, "The returned number replaces the value before the commit");

    reading = MakeReading(keeping, 7.0F);
    processor.Process(keeping, reading);
    zassert_false(reading.HasError());
    zassert_equal(reading.value.value(), 7.0F, "nil keeps the reading's value");

    reading = MakeReading(failing, 1.0F);
    processor.Process(failing, reading);
    zassert_true(reading.HasError());
    zassert_equal(reading.error, ReadingError::SCRIPT_FAILED);

    // A failed call leaves the state usable.
    reading = MakeReading(doubling, 1.5F);
    processor.Process(doubling, reading);
    zassert_equal(reading.value.value(), 3.0F);
}

ZTEST(lua_scripts, test_scripts_read_and_write_other_sensors_through_the_frame) {
    auto host = std::make_shared<LuaScript>(LuaScript::CreateExt());
    auto frame = std::make_shared<SensorReadingsFrame>();

    auto source = MakeUserSensor("source");
    auto target = MakeUserSensor("target");
    std::vector<std::shared_ptr<Sensor>> sensors { source, target };
    frame->Configure(sensors);

    GlobalFunctionsRegistry::RegisterGetSensorValue(*host, *frame);
    GlobalFunctionsRegistry::RegisterUpdateSensorValue(*host, *frame);

    auto runtime = MakeScriptedRuntime(host, "writer",
        "function post_process_sensor_value(id, value)\n"
        "  local other = get_sensor_value('source')\n"
        "  if other == nil then return -1 end\n"
        "  update_sensor_value('target', other + value)\n"
        "  return value\n"
        "end\n");

    ScriptProcessor processor(SensorScript::kPostProcessFunction);

    // Nothing processed yet: get_sensor_value returns nil.
    auto reading = MakeReading(runtime, 1.0F);
    processor.Process(runtime, reading);
    zassert_equal(reading.value.value(), -1.0F);

    SensorReading committed(source.get());
    committed.source = ReadingSource::PROCESSING;
    committed.value = 10.0F;
    committed.status = ReadingStatus::PROCESSED;
    zassert_true(frame->AddOrUpdateReading(committed));

    SensorReading target_reading(target.get());
    target_reading.source = ReadingSource::PROCESSING;
    target_reading.value = 0.0F;
    target_reading.status = ReadingStatus::PROCESSED;
    zassert_true(frame->AddOrUpdateReading(target_reading));

    reading = MakeReading(runtime, 5.0F);
    processor.Process(runtime, reading);
    zassert_equal(reading.value.value(), 5.0F);
    zassert_equal(frame->TryGetReadingValue("target").value(), 15.0F, "Written through update_sensor_value");
}

ZTEST(lua_scripts, test_a_script_past_the_memory_limit_fails_its_call_only) {
    auto host = std::make_shared<LuaScript>(LuaScript::CreateExt());
    host->SetMemoryLimit(64 * 1024);

    auto greedy = MakeScriptedRuntime(host, "greedy",
        "function create_sensor_value(id)\n"
        "  local t = {}\n"
        "  for i = 1, 1000000 do t[i] = i end\n"
        "  return #t\n"
        "end\n");
    auto modest = MakeScriptedRuntime(host, "modest",
        "function create_sensor_value(id) return 3 end\n");

    zassert_true(greedy.script.HasCreateValue());

    auto time_service = MakeTimeService();
    SensorReaderUserValueType greedy_reader(time_service, greedy);
    SensorReaderUserValueType modest_reader(time_service, modest);

    bool failed = false;
    try {
        (void)greedy_reader.Read();
    } catch(const std::runtime_error&) {
        failed = true;
    }
    zassert_true(failed, "The allocation past the limit failed the call");
    zassert_true(host->GetMemoryBudget().peak <= 64 * 1024 + 4096, "Peak %zu stayed near the limit", host->GetMemoryBudget().peak);

    // The table was collectable; the state and the other script keep working.
    zassert_equal(modest_reader.Read().value.value(), 3.0F);
    zassert_true(host->GetMemoryUsed() < 64 * 1024);
}
