#include <algorithm>
#include <exception>

#include <zephyr/logging/log.h>

#include "processing_scheduler_service.h"

namespace eerie_leap::domain::sensor_domain::services {

using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::sensor_domain::sensor_readers;

LOG_MODULE_DECLARE(processing_service_logger);

ProcessingSchedulerService::ProcessingSchedulerService(
    std::shared_ptr<ITimeService> time_service,
    std::shared_ptr<SensorReaderFactory> sensor_reader_factory,
    std::shared_ptr<WorkQueueThread> work_queue_thread,
    std::shared_ptr<ReadingPipeline> pipeline)
        : sensor_reader_factory_(std::move(sensor_reader_factory)),
        scheduler_(std::move(work_queue_thread), std::move(pipeline), std::move(time_service)) {};

void ProcessingSchedulerService::SetGeneration(std::shared_ptr<const SensorGeneration> generation) {
    generation_ = std::move(generation);
}

// Groups the generation's polled sensors by sampling rate, keeping the processing order inside a group.
// A sensor whose reader cannot be built is logged and left out; the others run.
std::vector<std::unique_ptr<RateGroup>> ProcessingSchedulerService::CreateRateGroups() const {
    std::vector<std::unique_ptr<RateGroup>> groups;

    for(const auto& runtime : generation_->runtimes) {
        if(runtime.update_method != SensorReadingUpdateMethod::SCHEDULER)
            continue;

        const auto& configuration = runtime.GetSensor().configuration;
        if(!configuration.sampling_rate_ms.has_value() || configuration.sampling_rate_ms.value() <= 0)
            continue;

        const int period_ms = configuration.sampling_rate_ms.value();

        RateGroup* group = nullptr;
        for(auto& candidate : groups) {
            if(candidate->period_ms == period_ms) {
                group = candidate.get();
                break;
            }
        }

        // The reader reserves its ADC slot in the group, so the group exists first.
        if(group == nullptr) {
            auto new_group = std::make_unique<RateGroup>();
            new_group->period_ms = period_ms;
            group = new_group.get();
            groups.push_back(std::move(new_group));
        }

        RateGroupBuildContext context{
            .adc_samples = group->adc_samples,
            .adc_configuration = generation_->adc_configuration.get()
        };

        std::unique_ptr<ISensorReader> reader;
        try {
            reader = sensor_reader_factory_->Create(runtime, context);
        } catch(const std::exception& e) {
            LOG_ERR("Sensor %s has no reader: %s", runtime.GetSensor().id.c_str(), e.what());
            continue;
        }

        if(reader == nullptr) {
            LOG_WRN("Sensor %s has no reader on this unit.", runtime.GetSensor().id.c_str());
            continue;
        }

        group->entries.push_back(RateGroupEntry{.runtime = &runtime, .reader = std::move(reader)});
    }

    // A group whose every sensor was left out has nothing to run.
    std::erase_if(groups, [](const auto& group) { return group->entries.empty(); });

    for(auto& group : groups)
        group->adc_samples.Seal();

    return groups;
}

bool ProcessingSchedulerService::DoStart() {
    if(generation_ == nullptr)
        return false;

    // Nothing is scheduled until every group exists, so a failed start only has to drop them and
    // the generation; Stop() does not reach a service that failed to start.
    try {
        scheduler_.SetGroups(CreateRateGroups());
        scheduler_.Start();
    } catch(...) {
        scheduler_.ClearGroups();
        generation_ = nullptr;
        throw;
    }

    return true;
}

// The groups go before the generation they refer to.
bool ProcessingSchedulerService::DoStop() {
    scheduler_.Stop();
    scheduler_.ClearGroups();
    generation_ = nullptr;

    return true;
}

bool ProcessingSchedulerService::DoPause() {
    scheduler_.Stop();

    return true;
}

// The groups restart on a fresh phase, one period from now: the paused time is not caught up on.
bool ProcessingSchedulerService::DoResume() {
    scheduler_.Start();

    return true;
}

} // namespace eerie_leap::domain::sensor_domain::services
