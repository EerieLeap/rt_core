#include <span>

#include "subsys/time/time_helpers.hpp"
#include "subsys/lua_script/lua_script.h"
#include "domain/script_domain/utilities/global_fuctions_registry.h"

#include "processing_scheduler_service.h"

namespace eerie_leap::domain::sensor_domain::services {

using namespace eerie_leap::subsys::time;
using namespace eerie_leap::subsys::lua_script;
using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::script_domain::utilities;

LOG_MODULE_DECLARE(processing_service_logger);

ProcessingSchedulerService::ProcessingSchedulerService(
    std::shared_ptr<SensorsConfigurationManager> sensors_configuration_manager,
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
    std::shared_ptr<SensorReaderFactory> sensor_reader_factory,
    std::shared_ptr<WorkQueueThread> work_queue_thread,
    std::shared_ptr<std::vector<std::shared_ptr<IReadingProcessor>>> reading_processors)
        : sensors_configuration_manager_(std::move(sensors_configuration_manager)),
        sensor_readings_frame_(std::move(sensor_readings_frame)),
        sensor_reader_factory_(std::move(sensor_reader_factory)),
        work_queue_thread_(std::move(work_queue_thread)),
        reading_processors_(std::move(reading_processors)) {};

WorkQueueTaskResult ProcessingSchedulerService::ProcessSensorWorkTask(SensorTask* task) {
    try {
        task->reader->Read();

        if(task->readings_frame->HasReading(task->sensor->id_hash)) {
            for(auto processor : *task->reading_processors)
                processor->ProcessReading(task->sensor->id_hash);

            auto reading_optional = task->readings_frame->TryGetReading(task->sensor->id_hash);
            if(reading_optional) {
                auto reading = std::move(reading_optional.value());
                if(reading.status < ReadingStatus::PROCESSED) {
                    reading.status = ReadingStatus::PROCESSED;
                    task->readings_frame->AddOrUpdateReading(reading);
                }

                LOG_DBG("Sensor Reading - ID: %s, Guid: %llu, Value: %.3f, Time: %s",
                    task->sensor->id.c_str(),
                    reading.id.AsUint64(),
                    reading.value.value_or(0.0f),
                    TimeHelpers::GetFormattedString(reading.timestamp.value()).c_str());
            }
        }
    } catch (const std::exception& e) {
        LOG_DBG("Error processing sensor: %s, Error: %s", task->sensor->id.c_str(), e.what());
    }

    return {
        .reschedule = true,
        .delay = task->sampling_rate_ms
    };
}

std::unique_ptr<SensorTask> ProcessingSchedulerService::CreateSensorTask(std::shared_ptr<Sensor> sensor) {
    auto reader = sensor_reader_factory_->Create(sensor);

    if(reader == nullptr)
        return nullptr;

    if(!sensor->configuration.sampling_rate_ms.has_value() || sensor->configuration.sampling_rate_ms.value() == 0)
        return nullptr;

    auto task = std::make_unique<SensorTask>();
    task->sampling_rate_ms = K_MSEC(sensor->configuration.sampling_rate_ms.value());
    task->sensor = sensor;
    task->readings_frame = sensor_readings_frame_;
    task->reading_processors = reading_processors_;
    task->reader = std::move(reader);

    if(sensor->configuration.expression_evaluator != nullptr) {
        sensor->configuration.expression_evaluator->RegisterVariableValueHandler(
            [&sensor_readings_frame = sensor_readings_frame_](const std::string& sensor_id) {
                return sensor_readings_frame->GetReadingValuePtr(sensor_id);
            });
    }

    return task;
}

void ProcessingSchedulerService::StartTasks() {
    for(auto& work_queue_task : work_queue_tasks_)
        work_queue_task.Schedule();
}

void ProcessingSchedulerService::CancelTasks() {
    for(auto& work_queue_task : work_queue_tasks_) {
        LOG_INF("Canceling task for sensor: %s", work_queue_task.GetUserdata()->sensor->id.c_str());

        while(work_queue_task.Cancel())
            k_sleep(K_MSEC(1));
    }
}

bool ProcessingSchedulerService::DoStart() {
    const auto* sensors = sensors_configuration_manager_->Get();

    work_queue_tasks_.clear();
    for(const auto& sensor : *sensors) {
        if(sensor->configuration.GetReadingUpdateMethod() != SensorReadingUpdateMethod::SCHEDULER)
            continue;

        auto task = CreateSensorTask(sensor);
        if(task == nullptr)
            continue;

        work_queue_tasks_.emplace_back(
            work_queue_thread_->CreateTask(ProcessSensorWorkTask, std::move(task)));
        LOG_INF("Created task for sensor: %s", sensor->id.c_str());
    }

    StartTasks();

    return true;
}

bool ProcessingSchedulerService::DoStop() {
    CancelTasks();
    work_queue_tasks_.clear();

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
