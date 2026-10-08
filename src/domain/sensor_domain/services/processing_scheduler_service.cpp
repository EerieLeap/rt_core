#include "domain/sensor_domain/processors/reading_pipeline.hpp"

#include "processing_scheduler_service.h"

namespace eerie_leap::domain::sensor_domain::services {

using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::sensor_domain::processors;

LOG_MODULE_DECLARE(processing_service_logger);

ProcessingSchedulerService::ProcessingSchedulerService(
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
    std::shared_ptr<SensorReaderFactory> sensor_reader_factory,
    std::shared_ptr<WorkQueueThread> work_queue_thread,
    std::shared_ptr<std::vector<std::shared_ptr<IReadingProcessor>>> reading_processors)
        : sensor_readings_frame_(std::move(sensor_readings_frame)),
        sensor_reader_factory_(std::move(sensor_reader_factory)),
        work_queue_thread_(std::move(work_queue_thread)),
        reading_processors_(std::move(reading_processors)) {};

void ProcessingSchedulerService::SetGeneration(std::shared_ptr<const SensorGeneration> generation) {
    generation_ = std::move(generation);
}

WorkQueueTaskResult ProcessingSchedulerService::ProcessSensorWorkTask(SensorTask* task) {
    const SensorRuntime& runtime = *task->runtime;

    SensorReading reading(runtime.sensor.get());
    reading.source = ReadingSource::PROCESSING;

    try {
        reading = task->reader->Read();
        ReadingPipeline::Run(*task->reading_processors, runtime, reading);
    } catch (const std::exception& e) {
        reading.SetError(ReadingError::READER_FAILED);

        LOG_DBG("Error reading sensor: %s, Error: %s", runtime.GetSensor().id.c_str(), e.what());
    }

    task->readings_frame->AddOrUpdateReading(reading);

    if(reading.HasError())
        LOG_DBG("Sensor %s: %s", runtime.GetSensor().id.c_str(), ToString(reading.error).data());

    return {
        .reschedule = true,
        .delay = task->sampling_rate_ms
    };
}

std::unique_ptr<SensorTask> ProcessingSchedulerService::CreateSensorTask(const SensorRuntime& runtime) {
    const auto& configuration = runtime.GetSensor().configuration;

    if(!configuration.sampling_rate_ms.has_value() || configuration.sampling_rate_ms.value() == 0)
        return nullptr;

    auto reader = sensor_reader_factory_->Create(runtime);
    if(reader == nullptr)
        return nullptr;

    auto task = std::make_unique<SensorTask>();
    task->sampling_rate_ms = K_MSEC(configuration.sampling_rate_ms.value());
    task->runtime = &runtime;
    task->readings_frame = sensor_readings_frame_;
    task->reading_processors = reading_processors_;
    task->reader = std::move(reader);

    return task;
}

void ProcessingSchedulerService::StartTasks() {
    for(auto& work_queue_task : work_queue_tasks_)
        work_queue_task.Schedule();
}

void ProcessingSchedulerService::CancelTasks() {
    for(auto& work_queue_task : work_queue_tasks_) {
        LOG_INF("Canceling task for sensor: %s", work_queue_task.GetUserdata()->runtime->GetSensor().id.c_str());

        while(work_queue_task.Cancel())
            k_sleep(K_MSEC(1));
    }
}

bool ProcessingSchedulerService::DoStart() {
    if(generation_ == nullptr)
        return false;

    work_queue_tasks_.clear();
    for(const auto& runtime : generation_->runtimes) {
        if(runtime.update_method != SensorReadingUpdateMethod::SCHEDULER)
            continue;

        auto task = CreateSensorTask(runtime);
        if(task == nullptr)
            continue;

        work_queue_tasks_.emplace_back(
            work_queue_thread_->CreateTask(ProcessSensorWorkTask, std::move(task)));
        LOG_INF("Created task for sensor: %s", runtime.GetSensor().id.c_str());
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
