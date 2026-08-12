#include <string>
#include <cstdio>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>

#include "subsys/time/time_helpers.hpp"
#include "subsys/random/rng.h"
#include "utilities/string/static_string.hpp"
#include "utilities/string/string_helpers.h"

#include "log_writer_service.h"

namespace eerie_leap::domain::logging_domain::services {

using namespace eerie_leap::subsys::time;
using namespace eerie_leap::subsys::random;
using namespace eerie_leap::utilities::string;

LOG_MODULE_REGISTER(log_writer_service_logger);

LogWriterService::LogWriterService(
    std::shared_ptr<IFsService> fs_service,
    std::shared_ptr<LoggingConfigurationManager> logging_configuration_manager,
    std::shared_ptr<ITimeService> time_service,
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame)
        : work_queue_thread_(nullptr),
        work_queue_task_(std::nullopt),
        fs_service_(std::move(fs_service)),
        logging_configuration_manager_(std::move(logging_configuration_manager)),
        time_service_(std::move(time_service)),
        sensor_readings_frame_(std::move(sensor_readings_frame)),
        logger_running_(ATOMIC_INIT(0)) {}

void LogWriterService::SetLogger(std::shared_ptr<ILogger<SensorReading>> logger) {
    logger_ = std::move(logger);
}

void LogWriterService::Initialize() {
    work_queue_thread_ = std::make_unique<WorkQueueThread>(
        "log_writer_service",
        thread_stack_size_,
        thread_priority_);
    work_queue_thread_->Initialize();

    auto task = std::make_unique<LogWriterTask>();
    task->time_service = time_service_;
    task->sensor_readings_frame = sensor_readings_frame_;

    work_queue_task_ = work_queue_thread_->CreateTask(ProcessWorkTask, std::move(task));

    LOG_INF("Log writer service initialized.");
}

StaticString<LogWriterService::FILE_PATH_MAX_LENGTH> LogWriterService::GetNewLogDataFilePath(const std::chrono::system_clock::time_point& tp) {
    StaticString<FILE_PATH_MAX_LENGTH> path;

    path += CONFIG_EERIE_LEAP_LOG_DATA_FILES_DIR;
    path += "/";
    path += CONFIG_EERIE_LEAP_LOG_DATA_FILE_PREFIX;
    if(strlen(CONFIG_EERIE_LEAP_LOG_DATA_FILE_PREFIX) > 0)
        path += "_";
    path += StringHelpers::UInt32ToStaticString(TimeHelpers::ToUint32(tp)).ToString();
    path += "_";
    path += StringHelpers::UInt16ToStaticString(Rng::Get<uint16_t>()).ToString();

    return path;
}

int LogWriterService::LogWriterStart() {
    if(atomic_get(&logger_running_))
        return -1;

    if(!logger_) {
        LOG_ERR("Logger is not available.");
        return -1;
    }

    if(!fs_service_->IsAvailable()) {
        LOG_ERR("SD card is not available.");
        return -1;
    }

    if(!fs_service_->Exists(CONFIG_EERIE_LEAP_LOG_DATA_FILES_DIR)) {
        if(!fs_service_->CreateDirectory(CONFIG_EERIE_LEAP_LOG_DATA_FILES_DIR)) {
            LOG_ERR("Failed to create %s directory", CONFIG_EERIE_LEAP_LOG_DATA_FILES_DIR);
            return -1;
        }
    }

    if(logging_configuration_manager_->Get()->logging_interval_ms < 10) {
        LOG_ERR("Logging interval cannot be less than 10 ms.");
        return -1;
    }

    auto start_time = time_service_->GetCurrentTime();
    StaticString<FILE_PATH_MAX_LENGTH> file_path;

    for(int i = 0; i < 10; i++) {
        auto new_file_path = GetNewLogDataFilePath(start_time);
        new_file_path += ".";
        new_file_path += logger_->GetFileExtension();

        if(!fs_service_->Exists(new_file_path.ToString())) {
            file_path = new_file_path;
            break;
        }
    }

    if(file_path.Empty()) {
        LOG_ERR("Failed to create log file name");
        return -1;
    }

    fs_stream_buf_ = std::make_unique<FsServiceStreamBuf>(
        fs_service_.get(),
        file_path.ToString(),
        FsServiceStreamBuf::OpenMode::Append);
    logger_->StartLogging(*fs_stream_buf_, start_time);

    LOG_INF("Logging started. Log file created: %s", file_path.CStr());

    work_queue_task_.value().GetUserdata()->logging_interval_ms =
        K_MSEC(logging_configuration_manager_->Get()->logging_interval_ms);
    work_queue_task_.value().GetUserdata()->start_time = start_time;
    work_queue_task_.value().GetUserdata()->logger = logger_;

    atomic_set(&logger_running_, 1);
    work_queue_task_.value().Schedule();

    return 0;
}

int LogWriterService::LogWriterStop() {
    if(!atomic_get(&logger_running_))
        return -1;

    work_queue_task_.value().Cancel();
    atomic_set(&logger_running_, 0);

    logger_->StopLogging();
    fs_stream_buf_->close();

    LOG_INF("Logging stopped.");

    return 0;
}

WorkQueueTaskResult LogWriterService::ProcessWorkTask(LogWriterTask* task) {
    auto time_now = task->time_service->GetCurrentTime();
    for(const auto& [sensor_id, reading] : task->sensor_readings_frame->GetProcessedReadings())
        task->logger->LogReading(time_now, reading);

    return {
        .reschedule = true,
        .delay = task->logging_interval_ms
    };
}

bool LogWriterService::IsRunning() const {
    return atomic_get(&logger_running_);
}

} // namespace eerie_leap::domain::logging_domain::services
