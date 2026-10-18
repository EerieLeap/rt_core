#include <cmath>
#include <stdexcept>

#include "domain/sensor_domain/models/reading_status.h"
#include "domain/sensor_domain/models/sensor_reading.h"
#include "sensor_reader_user_value_type.h"

namespace eerie_leap::domain::sensor_domain::sensor_readers {

using namespace eerie_leap::domain::sensor_domain::models;

SensorReaderUserValueType::SensorReaderUserValueType(
    std::shared_ptr<ITimeService> time_service,
    const SensorRuntime& runtime
) :
    SensorReaderBase(std::move(time_service), runtime) {

    const auto type = GetSensor().configuration.type;
    if(type != SensorType::USER_ANALOG && type != SensorType::USER_INDICATOR)
        throw std::runtime_error("Unsupported sensor type");
}

SensorReading SensorReaderUserValueType::Read(const TickContext& tick) {
    SensorReading reading = CreateReading(tick);

    const auto& script = runtime_->script;
    if(!script.HasCreateValue()) {
        // A blank sensor carries what a script last wrote, which went past every stage, so the
        // reading is final. Otherwise it gets its value, if any, from the stages.
        const float* written_value = runtime_->written_value;
        if(written_value != nullptr && !std::isnan(*written_value)) {
            reading.value = *written_value;
            reading.status = ReadingStatus::PROCESSED;
        } else {
            reading.status = ReadingStatus::UNINITIALIZED;
        }

        return reading;
    }

    auto guard = script.host->Lock();
    auto* state = script.host->GetState();

    script.host->PushRef(script.create_value);
    lua_pushstring(state, GetSensor().id.c_str());

    if(lua_pcall(state, 1, 1, 0) != LUA_OK) {
        lua_pop(state, 1);
        script.host->CollectStep();
        throw std::runtime_error("Failed to call create_sensor_value function");
    }

    if(!lua_isnumber(state, -1)) {
        lua_pop(state, 1);
        script.host->CollectStep();
        throw std::runtime_error("create_sensor_value function didn't return a number.");
    }

    auto value = static_cast<float>(lua_tonumber(state, -1));
    lua_pop(state, 1);
    script.host->CollectStep();

    reading.value = value;
    reading.raw_value = value;
    reading.status = ReadingStatus::RAW;

    return reading;
}

} // namespace eerie_leap::domain::sensor_domain::sensor_readers
