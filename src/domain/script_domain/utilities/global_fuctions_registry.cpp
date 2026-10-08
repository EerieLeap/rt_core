#include "utilities/string/string_helpers.h"

#include "global_fuctions_registry.h"

namespace eerie_leap::domain::script_domain::utilities {

using namespace eerie_leap::subsys::lua_script;
using namespace eerie_leap::domain::sensor_domain::utilities;
using eerie_leap::utilities::string::StringHelpers;

void GlobalFunctionsRegistry::RegisterGetSensorValue(LuaScript& lua_script, SensorReadingsFrame& sensor_readings_frame) {
    lua_script.RegisterGlobalFunction("get_sensor_value", &GlobalFunctionsRegistry::LuaGetSensorValue, &sensor_readings_frame);
}

void GlobalFunctionsRegistry::RegisterUpdateSensorValue(LuaScript& lua_script, SensorReadingsFrame& sensor_readings_frame) {
    lua_script.RegisterGlobalFunction("update_sensor_value", &GlobalFunctionsRegistry::LuaUpdateSensorValue, &sensor_readings_frame);
}

// get_sensor_value(sensor_id) -> the latest processed value, or nil
int GlobalFunctionsRegistry::LuaGetSensorValue(lua_State* state) {
    if(lua_gettop(state) != 1)
        return luaL_error(state, "Expected 1 argument");

    auto* sensor_readings_frame =
        static_cast<SensorReadingsFrame*>(lua_touserdata(state, lua_upvalueindex(1)));

    size_t length = 0;
    const char* sensor_id = luaL_checklstring(state, 1, &length);

    const auto value = sensor_readings_frame->TryGetReadingValue(StringHelpers::GetHash(std::string_view(sensor_id, length)));
    if(!value.has_value()) {
        lua_pushnil(state);
        return 1;
    }

    lua_pushnumber(state, value.value());

    return 1;
}

// update_sensor_value(sensor_id, value) -> whether the sensor exists and has a reading
int GlobalFunctionsRegistry::LuaUpdateSensorValue(lua_State* state) {
    if(lua_gettop(state) != 2)
        return luaL_error(state, "Expected 2 arguments");

    auto* sensor_readings_frame =
        static_cast<SensorReadingsFrame*>(lua_touserdata(state, lua_upvalueindex(1)));

    size_t length = 0;
    const char* sensor_id = luaL_checklstring(state, 1, &length);
    const auto value = static_cast<float>(luaL_checknumber(state, 2));

    const bool updated = sensor_readings_frame->UpdateReadingValue(
        StringHelpers::GetHash(std::string_view(sensor_id, length)), value);

    lua_pushboolean(state, updated);

    return 1;
}

}
