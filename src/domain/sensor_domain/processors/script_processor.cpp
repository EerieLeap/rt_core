#include <utility>

#include <zephyr/logging/log.h>

#include "subsys/lua_script/lua_script.h"

#include "script_processor.h"

namespace eerie_leap::domain::sensor_domain::processors {

using namespace eerie_leap::domain::sensor_domain::models;

LOG_MODULE_REGISTER(script_processor_logger);

ScriptProcessor::ScriptProcessor(std::string function_name)
    : function_name_(std::move(function_name)) {}

void ScriptProcessor::Process(const SensorRuntime& runtime, SensorReading& reading) {
    const auto& script = runtime.script;
    if(!script.HasPostProcess())
        return;

    const Sensor& sensor = runtime.GetSensor();

    auto guard = script.host->Lock();
    auto* state = script.host->GetState();

    script.host->PushRef(script.post_process);
    lua_pushstring(state, sensor.id.c_str());
    if(reading.value.has_value())
        lua_pushnumber(state, reading.value.value());
    else
        lua_pushnil(state);

    if(lua_pcall(state, 2, 1, 0) != LUA_OK) {
        const char* message = lua_tostring(state, -1);
        LOG_ERR("Sensor %s: %s failed: %s", sensor.id.c_str(), function_name_.c_str(), message != nullptr ? message : "");
        lua_pop(state, 1);
        script.host->CollectStep();

        reading.SetError(ReadingError::SCRIPT_FAILED);
        return;
    }

    if(lua_isnumber(state, -1))
        reading.value = static_cast<float>(lua_tonumber(state, -1));

    lua_pop(state, 1);
    script.host->CollectStep();
}

} // namespace eerie_leap::domain::sensor_domain::processors
