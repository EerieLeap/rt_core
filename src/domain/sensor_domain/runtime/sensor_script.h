#pragma once

#include <memory>

#include "subsys/lua_script/lua_script.h"

namespace eerie_leap::domain::sensor_domain::runtime {

using eerie_leap::subsys::lua_script::LuaScript;

// A sensor's script, loaded into the generation's shared Lua state. The environment keeps the
// script's globals private; the two entry points are resolved once when the generation is built.
// Every use of the host takes host->Lock() first.
struct SensorScript {
    static constexpr const char* kCreateValueFunction = "create_sensor_value";
    static constexpr const char* kPostProcessFunction = "post_process_sensor_value";

    std::shared_ptr<LuaScript> host;
    int environment = LuaScript::kNoRef;
    int create_value = LuaScript::kNoRef;   // create_sensor_value(sensor_id) -> number
    int post_process = LuaScript::kNoRef;   // post_process_sensor_value(sensor_id, value) -> number | nil

    [[nodiscard]] bool IsLoaded() const { return host != nullptr && environment != LuaScript::kNoRef; }
    [[nodiscard]] bool HasCreateValue() const { return IsLoaded() && create_value != LuaScript::kNoRef; }
    [[nodiscard]] bool HasPostProcess() const { return IsLoaded() && post_process != LuaScript::kNoRef; }
};

} // namespace eerie_leap::domain::sensor_domain::runtime
