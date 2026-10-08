#include <exception>

#include "processing_scheduler_service.h"

namespace eerie_leap::domain::sensor_domain::services {

using namespace eerie_leap::domain::sensor_domain::models;

LOG_MODULE_DECLARE(processing_service_logger);

ProcessingSchedulerService::ProcessingSchedulerService(
    std::shared_ptr<SensorReaderFactory> sensor_reader_factory,
    std::shared_ptr<WorkQueueThread> work_queue_thread,
    std::shared_ptr<ReadingPipeline> pipeline)
        : sensor_reader_factory_(std::move(sensor_reader_factory)),
        work_queue_thread_(std::move(work_queue_thread)),
        pipeline_(std::move(pipeline)) {};

void ProcessingSchedulerService::SetGeneration(std::shared_ptr<const SensorGeneration> generation) {
    generation_ = std::move(generation);
}

WorkQueueTaskResult ProcessingSchedulerService::ProcessRateGroup(RateGroupTask* task) {
    for(auto& entry : task->entries) {
        const SensorRuntime& runtime = *entry.runtime;

        SensorReading reading(runtime.sensor.get());
        reading.source = ReadingSource::PROCESSING;

        try {
            reading = entry.reader->Read();
        } catch (const std::exception& e) {
            reading.SetError(ReadingError::READER_FAILED);

            LOG_DBG("Error reading sensor: %s, Error: %s", runtime.GetSensor().id.c_str(), e.what());
        }

        task->pipeline->Process(runtime, reading);

        if(reading.HasError())
            LOG_DBG("Sensor %s: %s", runtime.GetSensor().id.c_str(), ToString(reading.error).data());
    }

    return {
        .reschedule = true,
        .delay = task->period
    };
}

// Groups the generation's polled sensors by sampling rate, keeping the processing order inside a group.
std::vector<std::unique_ptr<RateGroupTask>> ProcessingSchedulerService::CreateRateGroups() const {
    std::vector<std::unique_ptr<RateGroupTask>> groups;

    for(const auto& runtime : generation_->runtimes) {
        if(runtime.update_method != SensorReadingUpdateMethod::SCHEDULER)
            continue;

        const auto& configuration = runtime.GetSensor().configuration;
        if(!configuration.sampling_rate_ms.has_value() || configuration.sampling_rate_ms.value() <= 0)
            continue;

        auto reader = sensor_reader_factory_->Create(runtime);
        if(reader == nullptr) {
            LOG_WRN("Sensor %s has no reader on this unit.", runtime.GetSensor().id.c_str());
            continue;
        }

        const int period_ms = configuration.sampling_rate_ms.value();

        RateGroupTask* group = nullptr;
        for(auto& candidate : groups) {
            if(candidate->period_ms == period_ms) {
                group = candidate.get();
                break;
            }
        }

        if(group == nullptr) {
            auto new_group = std::make_unique<RateGroupTask>();
            new_group->period = K_MSEC(period_ms);
            new_group->period_ms = period_ms;
            new_group->pipeline = pipeline_;
            group = new_group.get();
            groups.push_back(std::move(new_group));
        }

        group->entries.push_back(RateGroupEntry{.runtime = &runtime, .reader = std::move(reader)});
    }

    return groups;
}

void ProcessingSchedulerService::StartTasks() {
    for(auto& work_queue_task : work_queue_tasks_)
        work_queue_task.Schedule();
}

void ProcessingSchedulerService::CancelTasks() {
    for(auto& work_queue_task : work_queue_tasks_) {
        LOG_INF("Canceling the %d ms rate group.", work_queue_task.GetUserdata()->period_ms);

        while(work_queue_task.Cancel())
            k_sleep(K_MSEC(1));
    }
}

bool ProcessingSchedulerService::DoStart() {
    if(generation_ == nullptr)
        return false;

    work_queue_tasks_.clear();
    for(auto& group : CreateRateGroups()) {
        LOG_INF("Rate group %d ms reads %zu sensors.", group->period_ms, group->entries.size());

        work_queue_tasks_.emplace_back(
            work_queue_thread_->CreateTask(ProcessRateGroup, std::move(group)));
    }

    StartTasks();

    return true;
}

// Tasks go before the generation they refer to.
bool ProcessingSchedulerService::DoStop() {
    CancelTasks();
    work_queue_tasks_.clear();
    generation_ = nullptr;

    return true;
}

bool ProcessingSchedulerService::DoPause() {
    CancelTasks();

    return true;
}

bool ProcessingSchedulerService::DoResume() {
    StartTasks();

    return true;
}

} // namespace eerie_leap::domain::sensor_domain::services
