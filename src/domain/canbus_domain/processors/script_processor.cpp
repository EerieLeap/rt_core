#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "script_processor.h"

namespace eerie_leap::domain::canbus_domain::processors {

ScriptProcessor::ScriptProcessor(
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
    std::string function_name
) :
    sensor_readings_frame_(std::move(sensor_readings_frame)),
    function_name_(std::move(function_name)) {}

std::vector<uint8_t> ScriptProcessor::Process(
    const CanMessageConfiguration& message_configuration,
    std::span<const uint8_t> can_frame_data
) {

    const auto lua_script = message_configuration.lua_script;
    if(lua_script == nullptr)
        return {};

    auto* state = lua_script->GetState();
    if(state == nullptr)
        return {};

    lua_getglobal(state, function_name_.c_str());

    if(!lua_isfunction(state, -1)) {
        lua_pop(state, 1);
        return {};
    }

    lua_pushinteger(state, static_cast<lua_Integer>(message_configuration.frame_id)); // CAN IDs are at most 29 bits.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the Lua C API takes the frame bytes as char.
    lua_pushlstring(state, reinterpret_cast<const char*>(can_frame_data.data()), can_frame_data.size());

    if(lua_pcall(state, 2, 1, 0) != LUA_OK) {
        lua_pop(state, 1);
        throw std::runtime_error("Error calling Lua function: " + std::string(lua_tostring(state, -1)));
    }

    if(lua_isstring(state, -1) == 0) {
        lua_pop(state, 1);
        throw std::runtime_error("Function didn't return a string (binary data).");
    }

    size_t data_len = 0;
    const char* data_ptr = lua_tolstring(state, -1, &data_len);

    const std::string_view data(data_ptr, data_len);
    auto result = std::vector<uint8_t>(data.begin(), data.end());

    lua_pop(state, 1);

    return result;
}

} // namespace eerie_leap::domain::canbus_domain::processors
