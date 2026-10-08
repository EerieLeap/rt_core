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
    auto* lua_script = runtime.lua_script.get();
    if(lua_script == nullptr)
        return;

    auto* state = lua_script->GetState();
    if(state == nullptr)
        return;

    const Sensor& sensor = runtime.GetSensor();

    lua_getglobal(state, function_name_.c_str());

    if(!lua_isfunction(state, -1)) {
        lua_pop(state, 1);
        return;
    }

    lua_pushstring(state, sensor.id.c_str());

    if(lua_pcall(state, 1, 0, 0) != LUA_OK) {
        const char* message = lua_tostring(state, -1);
        LOG_ERR("Sensor %s: %s failed: %s", sensor.id.c_str(), function_name_.c_str(), message != nullptr ? message : "");
        lua_pop(state, 1);

        reading.SetError(ReadingError::SCRIPT_FAILED);
    }
}

} // namespace eerie_leap::domain::sensor_domain::processors
