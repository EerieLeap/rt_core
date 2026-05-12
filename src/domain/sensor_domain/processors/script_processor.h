#pragma once

#include <memory>
#include <string>

#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "domain/sensor_domain/processors/i_reading_processor.h"

namespace eerie_leap::domain::sensor_domain::processors {

using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;

// NOTE: calls lua function named according to function_name_ argument
// with reading string sensor id as argument
// and returns float reading value
//
// function process_reading(sensor_id)
//     return 8.1234
// end

class ScriptProcessor : public IReadingProcessor {
private:
    std::string function_name_;
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame_;

public:
    explicit ScriptProcessor(const std::string& function_name, std::shared_ptr<SensorReadingsFrame> sensor_readings_frame);

    void ProcessReading(const size_t sensor_id_hash) override;
};

} // namespace eerie_leap::domain::sensor_domain::processors
