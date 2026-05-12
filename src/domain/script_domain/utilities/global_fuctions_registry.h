#pragma once

#include "subsys/lua_script/lua_script.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"

namespace eerie_leap::domain::script_domain::utilities {

using eerie_leap::subsys::lua_script::LuaScript;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;

class GlobalFunctionsRegistry {
private:
    static int LuaGetSensorValue(lua_State* state);
    static int LuaUpdateSensorValue(lua_State* state);

public:
    static void RegisterGetSensorValue(LuaScript& lua_script, SensorReadingsFrame& sensor_readings_frame);
    static void RegisterUpdateSensorValue(LuaScript& lua_script, SensorReadingsFrame& sensor_readings_frame);
};

}
