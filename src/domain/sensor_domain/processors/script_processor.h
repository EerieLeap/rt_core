#pragma once

#include <string>

#include "domain/sensor_domain/processors/i_reading_processor.h"

namespace eerie_leap::domain::sensor_domain::processors {

// NOTE: calls the Lua function named according to function_name_ argument
// with the reading's string sensor id as argument. The function returns nothing;
// it changes values through update_sensor_value(sensor_id, value).
//
// function post_process_sensor_value(sensor_id)
//     update_sensor_value(sensor_id, get_sensor_value(sensor_id) * 2)
// end

class ScriptProcessor : public IReadingProcessor {
private:
    std::string function_name_;

public:
    explicit ScriptProcessor(std::string function_name);

    void Process(const SensorRuntime& runtime, SensorReading& reading) override;
};

} // namespace eerie_leap::domain::sensor_domain::processors
