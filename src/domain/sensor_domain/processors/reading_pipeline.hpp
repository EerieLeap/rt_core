#pragma once

#include <memory>
#include <vector>

#include "domain/sensor_domain/models/reading_status.h"
#include "i_reading_processor.h"

namespace eerie_leap::domain::sensor_domain::processors {

using eerie_leap::domain::sensor_domain::models::ReadingStatus;

class ReadingPipeline {
public:
    // Runs the stages on a reading the caller owns, stopping at the first failure, and marks a
    // reading that went through as PROCESSED. The caller commits the result once.
    static void Run(
        const std::vector<std::shared_ptr<IReadingProcessor>>& processors,
        const Sensor& sensor,
        SensorReading& reading) {

        for(const auto& processor : processors) {
            if(reading.HasError())
                return;

            processor->Process(sensor, reading);
        }

        if(reading.status < ReadingStatus::PROCESSED)
            reading.status = ReadingStatus::PROCESSED;
    }
};

} // namespace eerie_leap::domain::sensor_domain::processors
