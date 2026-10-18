#pragma once

#include <algorithm>
#include <chrono>
#include <memory>
#include <vector>

#include <zephyr/kernel.h>

#include "domain/sensor_domain/models/reading_source.h"
#include "domain/sensor_domain/models/reading_status.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "i_reading_processor.h"
#include "subsys/threading/work_queue_thread.h"
#include "subsys/time/i_time_service.h"

namespace eerie_leap::domain::sensor_domain::processors {

using eerie_leap::domain::sensor_domain::models::ReadingSource;
using eerie_leap::domain::sensor_domain::models::ReadingStatus;
using eerie_leap::domain::sensor_domain::runtime::SensorGeneration;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::subsys::time::ITimeService;

// Takes a reading from its source to the frame: the stages, the indicator conditioning, one
// commit, and the sensors that are derived from it. Every source (CAN route, GPIO edge, rate
// group) ends in Process().
class ReadingPipeline {
private:
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame_;
    std::shared_ptr<std::vector<std::shared_ptr<IReadingProcessor>>> processors_;
    std::shared_ptr<ITimeService> time_service_;
    std::shared_ptr<WorkQueueThread> work_queue_thread_;

    // Dependents are listed transitively and in processing order, so one pass gives each of them
    // fresh inputs exactly once. They carry the time of the reading they were derived from, so a
    // tick's dependents share its timestamp and a CAN-routed input's dependents its frame time.
    // A dependent whose conditioner holds a change is not committed. The sensors derived from it
    // are still evaluated in this pass, against its old value, and evaluated again when it settles.
    void EvaluateDependents(const SensorRuntime& runtime, const SensorReading& reading) {
        for(const auto* dependent : runtime.dependents) {
            SensorReading derived(dependent->sensor.get());
            derived.source = ReadingSource::PROCESSING;
            derived.timestamp = reading.timestamp.has_value() ? reading.timestamp : time_service_->GetCurrentTime();

            Run(*processors_, *dependent, derived);

            if(dependent->conditioner != nullptr && !dependent->conditioner->Offer(*this, *dependent, derived))
                continue;

            sensor_readings_frame_->AddOrUpdateReading(derived);
        }
    }

public:
    ReadingPipeline(
        std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
        std::shared_ptr<std::vector<std::shared_ptr<IReadingProcessor>>> processors,
        std::shared_ptr<ITimeService> time_service,
        // Where a held indicator change settles. Without one a held change settles only when a
        // later reading confirms it after its window, which is enough for a pipeline driven by hand.
        std::shared_ptr<WorkQueueThread> work_queue_thread = nullptr
    ) :
        sensor_readings_frame_(std::move(sensor_readings_frame)),
        processors_(std::move(processors)),
        time_service_(std::move(time_service)),
        work_queue_thread_(std::move(work_queue_thread)) {}

    // Runs the stages on a reading the caller owns, stopping at the first failure, and marks a
    // reading that went through as PROCESSED. Does not touch the frame.
    static void
    Run(const std::vector<std::shared_ptr<IReadingProcessor>>& processors,
        const SensorRuntime& runtime,
        SensorReading& reading) {

        for(const auto& processor : processors) {
            if(reading.HasError())
                return;

            processor->Process(runtime, reading);
        }

        reading.status = std::max(reading.status, ReadingStatus::PROCESSED);
    }

    // Runs the stages, conditions an indicator, commits the reading, and evaluates the sensors
    // derived from it. A failed reading is committed as it is and its dependents keep their
    // previous values. A change the conditioner holds is not committed now; the conditioner
    // commits it once it has settled. A reading that arrives PROCESSED (a blank USER sensor
    // carrying what a script wrote past the stages and the conditioner) is committed as it is.
    void Process(const SensorRuntime& runtime, SensorReading& reading) {
        if(reading.status == ReadingStatus::PROCESSED) {
            Commit(runtime, reading);
            return;
        }

        Run(*processors_, runtime, reading);

        if(runtime.conditioner != nullptr && !runtime.conditioner->Offer(*this, runtime, reading))
            return;

        Commit(runtime, reading);
    }

    // Commits a reading that went through the stages and evaluates its dependents. Process() ends
    // here, and so do a settled indicator change and a switch at start. Processing work queue.
    void Commit(const SensorRuntime& runtime, const SensorReading& reading) {
        sensor_readings_frame_->AddOrUpdateReading(reading);

        if(!reading.HasError())
            EvaluateDependents(runtime, reading);
    }

    // Commits the switch of every Toggle indicator, with its dependents, before its input has a
    // reading. Not through the conditioner, whose first reading still sets its input. Processing
    // work queue, before the sources start.
    void CommitSwitchStates(const SensorGeneration& generation) {
        const auto timestamp = time_service_->GetCurrentTime();

        for(const auto& runtime : generation.runtimes) {
            if(runtime.conditioner == nullptr || !runtime.conditioner->IsSwitch())
                continue;

            SensorReading reading(runtime.sensor.get());
            reading.source = ReadingSource::PROCESSING;
            reading.status = ReadingStatus::PROCESSED;
            reading.timestamp = timestamp;
            reading.value = runtime.conditioner->IsSwitchOn() ? 1.0F : 0.0F;

            Commit(runtime, reading);
        }
    }

    /** @return The queue held changes settle on, null without one. Only while it runs. */
    [[nodiscard]] k_work_q* GetWorkQueue() const {
        return work_queue_thread_ != nullptr ? work_queue_thread_->GetWorkQueue() : nullptr;
    }

    [[nodiscard]] std::chrono::system_clock::time_point GetCurrentTime() const {
        return time_service_->GetCurrentTime();
    }
};

} // namespace eerie_leap::domain::sensor_domain::processors
