#pragma once

#include <string>

#include "domain/sensor_domain/processors/i_reading_processor.h"

namespace eerie_leap::domain::sensor_domain::processors {

// Calls the script's post-processing function with the reading being processed:
//
//     function post_process_sensor_value(sensor_id, value)
//         return value * 2          -- the new value; nil keeps the reading's value
//     end
//
// The result is written into the reading before it is committed, so the script sees and changes
// the sample itself. update_sensor_value(sensor_id, value) remains for writing other sensors.
class ScriptProcessor : public IReadingProcessor {
private:
    std::string function_name_;

public:
    explicit ScriptProcessor(std::string function_name);

    void Process(const SensorRuntime& runtime, SensorReading& reading) override;
};

} // namespace eerie_leap::domain::sensor_domain::processors
