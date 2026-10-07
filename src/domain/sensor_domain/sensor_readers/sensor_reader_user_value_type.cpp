#include <stdexcept>

#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/sensor_domain/models/reading_status.h"
#include "sensor_reader_user_value_type.h"

namespace eerie_leap::domain::sensor_domain::sensor_readers {

using namespace eerie_leap::domain::sensor_domain::models;

SensorReaderUserValueType::SensorReaderUserValueType(
    std::shared_ptr<ITimeService> time_service,
    std::shared_ptr<Sensor> sensor)
        : SensorReaderBase(std::move(time_service), std::move(sensor)) {

    if(sensor_->configuration.type != SensorType::USER_ANALOG && sensor_->configuration.type != SensorType::USER_INDICATOR)
        throw std::runtime_error("Unsupported sensor type");

    has_create_reading_function_ = false;

    if(sensor_->configuration.lua_script == nullptr)
        return;

    lua_getglobal(sensor_->configuration.lua_script->GetState(), "create_sensor_value");

    has_create_reading_function_ = lua_isfunction(sensor_->configuration.lua_script->GetState(), -1);
    lua_pop(sensor_->configuration.lua_script->GetState(), 1);
}

SensorReading SensorReaderUserValueType::Read() {
    SensorReading reading = CreateReading();

    if(!has_create_reading_function_) {
        reading.status = ReadingStatus::UNINITIALIZED;

        return reading;
    }

    auto* state = sensor_->configuration.lua_script->GetState();

    lua_getglobal(state, "create_sensor_value");

    if(!lua_isfunction(state, -1)) {
        lua_pop(state, 1);
        throw std::runtime_error("create_sensor_value is not a function");
    }

    lua_pushstring(state, sensor_->id.c_str());

    if(lua_pcall(state, 1, 1, 0) != LUA_OK) {
        lua_pop(state, 1);
        throw std::runtime_error("Failed to call create_sensor_value function");
    }

    if(!lua_isnumber(state, -1)) {
        lua_pop(state, 1);
        throw std::runtime_error("create_sensor_value function didn't return a number.");
    }

    auto value = static_cast<float>(lua_tonumber(state, -1));
    lua_pop(state, 1);

    reading.value = value;
    reading.raw_value = value;
    reading.status = ReadingStatus::RAW;

    return reading;
}

} // namespace eerie_leap::domain::sensor_domain::sensor_readers
