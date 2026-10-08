#pragma once

#include <memory>
#include <vector>

#include "subsys/time/i_time_service.h"
#include "domain/sensor_domain/models/reading_status.h"
#include "domain/sensor_domain/models/reading_source.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "i_reading_processor.h"

namespace eerie_leap::domain::sensor_domain::processors {

using eerie_leap::subsys::time::ITimeService;
using eerie_leap::domain::sensor_domain::models::ReadingStatus;
using eerie_leap::domain::sensor_domain::models::ReadingSource;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;

// Takes a reading from its source to the frame: the stages, one commit, and the sensors that are
// derived from it. Every source (CAN route, GPIO edge, rate group) ends in Process().
class ReadingPipeline {
private:
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame_;
    std::shared_ptr<std::vector<std::shared_ptr<IReadingProcessor>>> processors_;
    std::shared_ptr<ITimeService> time_service_;

    // Dependents are listed transitively and in processing order, so one pass gives each of them
    // fresh inputs exactly once.
    void EvaluateDependents(const SensorRuntime& runtime) {
        for(const auto* dependent : runtime.dependents) {
            SensorReading derived(dependent->sensor.get());
            derived.source = ReadingSource::PROCESSING;
            derived.timestamp = time_service_->GetCurrentTime();

            Run(*processors_, *dependent, derived);
            sensor_readings_frame_->AddOrUpdateReading(derived);
        }
    }

public:
    ReadingPipeline(
        std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
        std::shared_ptr<std::vector<std::shared_ptr<IReadingProcessor>>> processors,
        std::shared_ptr<ITimeService> time_service)
            : sensor_readings_frame_(std::move(sensor_readings_frame)),
            processors_(std::move(processors)),
            time_service_(std::move(time_service)) {}

    // Runs the stages on a reading the caller owns, stopping at the first failure, and marks a
    // reading that went through as PROCESSED. Does not touch the frame.
    static void Run(
        const std::vector<std::shared_ptr<IReadingProcessor>>& processors,
        const SensorRuntime& runtime,
        SensorReading& reading) {

        for(const auto& processor : processors) {
            if(reading.HasError())
                return;

            processor->Process(runtime, reading);
        }

        if(reading.status < ReadingStatus::PROCESSED)
            reading.status = ReadingStatus::PROCESSED;
    }

    // Runs the stages, commits the reading, and evaluates the sensors derived from it. A failed
    // reading is committed as it is and its dependents keep their previous values.
    void Process(const SensorRuntime& runtime, SensorReading& reading) {
        Run(*processors_, runtime, reading);
        sensor_readings_frame_->AddOrUpdateReading(reading);

        if(!reading.HasError())
            EvaluateDependents(runtime);
    }
};

} // namespace eerie_leap::domain::sensor_domain::processors
