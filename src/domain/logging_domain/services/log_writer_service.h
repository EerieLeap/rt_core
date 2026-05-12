#pragma once

#include <memory>
#include <chrono>
#include <span>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "subsys/fs/services/i_fs_service.h"
#include "subsys/fs/services/fs_service_stream_buf.h"
#include "subsys/threading/work_queue_task_result.h"
#include "subsys/threading/work_queue_thread.h"
#include "subsys/time/i_time_service.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "domain/logging_domain/configuration/logging_configuration_manager.h"
#include "domain/logging_domain/loggers/i_logger.h"

#include "log_writer_task.hpp"

namespace eerie_leap::domain::logging_domain::services {

namespace threading = eerie_leap::subsys::threading;

using eerie_leap::subsys::fs::services::IFsService;
using eerie_leap::subsys::fs::services::FsServiceStreamBuf;
using threading::WorkQueueThread;
using threading::WorkQueueTask;
using threading::WorkQueueTaskResult;
using eerie_leap::subsys::time::ITimeService;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;
using eerie_leap::domain::logging_domain::configuration::LoggingConfigurationManager;
using eerie_leap::domain::logging_domain::loggers::ILogger;

class LogWriterService {
private:
    static constexpr int thread_stack_size_ = CONFIG_EERIE_LEAP_LOG_WRITER_STACK_SIZE;
    static constexpr int thread_priority_ = 8;
    std::unique_ptr<WorkQueueThread> work_queue_thread_;
    std::optional<threading::WorkQueueTask<LogWriterTask>> work_queue_task_;

    std::shared_ptr<IFsService> fs_service_;
    std::shared_ptr<LoggingConfigurationManager> logging_configuration_manager_;
    std::unique_ptr<FsServiceStreamBuf> fs_stream_buf_;
    std::shared_ptr<ITimeService> time_service_;
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame_;
    std::shared_ptr<ILogger<SensorReading>> logger_;

    atomic_t logger_running_;

    static WorkQueueTaskResult ProcessWorkTask(LogWriterTask* task);
    static std::string GetNewLogDataFileName(const time_point& tp);

public:
    LogWriterService(
        std::shared_ptr<IFsService> fs_service,
        std::shared_ptr<LoggingConfigurationManager> logging_configuration_manager,
        std::shared_ptr<ITimeService> time_service,
        std::shared_ptr<SensorReadingsFrame> sensor_readings_frame);
    ~LogWriterService() = default;

    void Initialize();
    void SetLogger(std::shared_ptr<ILogger<SensorReading>> logger);

    int LogWriterStart();
    int LogWriterStop();

    bool IsRunning() const;
};

} // namespace eerie_leap::domain::logging_domain::services
