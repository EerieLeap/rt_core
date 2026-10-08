#pragma once

#include <memory>

#include "subsys/lua_script/lua_script.h"
#include "sensor_reader_base.h"

namespace eerie_leap::domain::sensor_domain::sensor_readers {

using eerie_leap::subsys::lua_script::LuaScript;

class SensorReaderUserValueType : public SensorReaderBase {
private:
    bool has_create_reading_function_;

public:
    SensorReaderUserValueType(
        std::shared_ptr<ITimeService> time_service,
        const SensorRuntime& runtime);

    ~SensorReaderUserValueType() override = default;

    SensorReading Read() override;
};

} // namespace eerie_leap::domain::sensor_domain::sensor_readers
