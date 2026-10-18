#pragma once

#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "subsys/lua_script/lua_script.h"

namespace eerie_leap::domain::script_domain::utilities {

using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;
using eerie_leap::subsys::lua_script::LuaScript;

class GlobalFunctionsRegistry {
private:
    static int LuaGetSensorValue(lua_State* state);
    static int LuaUpdateSensorValue(lua_State* state);

public:
    static void RegisterGetSensorValue(LuaScript& lua_script, SensorReadingsFrame& sensor_readings_frame);
    static void RegisterUpdateSensorValue(LuaScript& lua_script, SensorReadingsFrame& sensor_readings_frame);
};

} // namespace eerie_leap::domain::script_domain::utilities
