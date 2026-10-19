#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "domain/logging_domain/configuration/logging_configuration_manager.h"
#include "domain/logging_domain/event_bus/logging_status_reason.h"
#include "domain/logging_domain/loggers/i_logger.h"
#include "domain/logging_domain/models/log_session_info.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "subsys/fs/services/fs_service_stream_buf.h"
#include "subsys/fs/services/i_fs_service.h"
#include "subsys/threading/service_base.h"
#include "subsys/threading/work_queue_task_result.h"
#include "subsys/threading/work_queue_thread.h"
#include "subsys/time/i_time_service.h"

#include "log_end_times.h"
#include "log_file_naming.h"
#include "log_writer_task.hpp"

namespace eerie_leap::domain::logging_domain::services {

namespace threading = eerie_leap::subsys::threading;

using eerie_leap::domain::logging_domain::configuration::LoggingConfigurationManager;
using eerie_leap::domain::logging_domain::event_bus::LoggingStatusReason;
using eerie_leap::domain::logging_domain::loggers::ILogger;
using eerie_leap::domain::logging_domain::models::LogSessionInfo;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;
using eerie_leap::subsys::fs::services::FsServiceStreamBuf;
using eerie_leap::subsys::fs::services::IFsService;
using eerie_leap::subsys::time::ITimeService;
using threading::ServiceBase;
using threading::ServiceState;
using threading::WorkQueueTask;
using threading::WorkQueueTaskResult;
using threading::WorkQueueThread;

// Writes the readings frame to a log file every logging_interval_ms on its own thread. Start() and
// Stop() publish StatusUpdated on logging_events, and so does a session that fails: the writer
// thread then stops writing and leaves the Stop() to whoever supervises it (LogWriterSupervisor),
// which stops it from the configuration work queue (Stop() waits for a running tick).
class LogWriterService final : public ServiceBase<> {
public:
    // Called by every Start(), so each file is laid out for the configurations in effect then.
    using LoggerFactory = std::function<std::shared_ptr<ILogger<SensorReading>>()>;

private:
    static constexpr int THREAD_STACK_SIZE = CONFIG_EERIE_LEAP_LOG_WRITER_STACK_SIZE;
    static constexpr int THREAD_PRIORITY = 8;
    std::unique_ptr<WorkQueueThread> work_queue_thread_;
    std::optional<threading::WorkQueueTask<LogWriterTask>> work_queue_task_;

    std::shared_ptr<IFsService> fs_service_;
    std::shared_ptr<LoggingConfigurationManager> logging_configuration_manager_;
    std::unique_ptr<FsServiceStreamBuf> fs_stream_buf_;
    std::shared_ptr<ITimeService> time_service_;
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame_;
    LoggerFactory logger_factory_;
    std::shared_ptr<ILogger<SensorReading>> logger_;
    StaticString<LogFileNaming::PATH_MAX_LENGTH> file_path_; // of the file last opened; the writer thread's

    // The file the writer has open, for IsOpenFile() on other threads. Held for a copy or a compare
    // of the path, never across file I/O.
    mutable k_mutex open_file_mutex_{};
    StaticString<LogFileNaming::PATH_MAX_LENGTH> open_file_path_;
    // The session, for GetSessionInfo(), under the same lock: set by Start(), cleared by Stop(); the
    // open file's bytes are the writer thread's, once per tick.
    bool is_session_active_ = false;
    int64_t session_since_ms_ = 0;
    uint64_t open_file_bytes_ = 0;

    // Where a finalized file's end is recorded; null records nothing. Set before Start().
    std::shared_ptr<LogEndTimes> end_times_;
    // Start of the file last opened, the end of one that closed before its first tick.
    std::chrono::system_clock::time_point file_start_time_;

    // Set by a tick that ended the session, cleared by the next Start().
    std::atomic<bool> has_failed_ = false;

    std::optional<uint64_t> max_log_size_bytes_override_;
    uint64_t min_free_space_bytes_ = static_cast<uint64_t>(CONFIG_EERIE_LEAP_LOG_MIN_FREE_SPACE_MB) * 1024 * 1024;

    WorkQueueTaskResult ProcessTick(LogWriterTask& task);
    // Ends the session from the writer thread: publishes why and schedules no further tick.
    WorkQueueTaskResult EndSession(LoggingStatusReason reason);
    // Closes the full file and opens the next one; the reason to end the session when it cannot.
    std::optional<LoggingStatusReason> RotateFile(LogWriterTask& task);

    bool HasMinimumFreeSpace() const;
    // Names, opens and starts a new file; on failure nothing is left open and no file behind.
    bool OpenLogFile(const std::chrono::system_clock::time_point& start_time);
    // Finalizes what it can and closes the open file; false when it could not be finalized.
    bool CloseLogFile(LogWriterTask& task);
    void SetOpenFile(std::string_view path);
    void SetSession(bool is_active, int64_t since_ms = 0);
    void SetOpenFileBytes(uint64_t bytes);
    // Records the end of the file just finalized; an index failure never fails the close.
    void RecordEndTime(const LogWriterTask& task);
    void ClearOpenFile();
    // The reason goes with IsActive = false only.
    static void PublishStatus(bool is_active, LoggingStatusReason reason = LoggingStatusReason::Stopped);

    bool DoInitialize() override;
    bool DoStart() override;
    bool DoStop() override;

public:
    LogWriterService(
        std::shared_ptr<IFsService> fs_service,
        std::shared_ptr<LoggingConfigurationManager> logging_configuration_manager,
        std::shared_ptr<ITimeService> time_service,
        std::shared_ptr<SensorReadingsFrame> sensor_readings_frame
    );
    ~LogWriterService() override = default;

    /** Only while stopped, and before anything can start the service: Start() reads it unguarded. */
    void SetLoggerFactory(LoggerFactory logger_factory);

    /** Counters since the last Start(); zero before the first one. */
    [[nodiscard]] LogWriterStats GetStats() const;

    /**
     * Whether the session ended on a write, rotation or free-space failure. The service is still
     * RUNNING then, without a tick, until Stop() closes the file; the next Start() clears it.
     */
    [[nodiscard]] bool HasFailed() const {
        return has_failed_.load();
    }

    /**
     * Whether @p path, relative to the volume, is the file the writer has open now, compared ASCII
     * case-insensitively. A stopped writer, and one between two files of a rotation, has none. Any
     * thread; takes a lock that is never held across file I/O.
     */
    [[nodiscard]] bool IsOpenFile(std::string_view path) const;

    /**
     * Where the end of every file this writer finalizes is recorded (normal stop and rotation; not a
     * file that could not be finalized). Only while stopped, and before anything can start the
     * service. The index outlives its use by being shared.
     */
    void SetEndTimes(std::shared_ptr<LogEndTimes> end_times);

    [[nodiscard]] std::shared_ptr<LogEndTimes> GetEndTimes() const {
        return end_times_;
    }

    /**
     * The running session, or null when none runs: before Start() succeeded and after Stop(). A
     * session that failed still runs until the supervisor's Stop(), and is returned with an empty
     * file name once the failure closed the file, else with the file the failure left open.
     * `since_ms` is the Start() time of the time service (Unix ms, UTC, the clock the file names
     * use); `file_name` is the open file's name without directory, its name as an asset of the
     * `logs` kind, empty between the two files of a rotation; `file_bytes` its bytes so far,
     * headers included, as of the last tick. Any thread, the MCUmgr work queue included: it
     * allocates nothing, does no file I/O and takes the lock of IsOpenFile(), never held across I/O.
     */
    [[nodiscard]] std::optional<LogSessionInfo> GetSessionInfo() const;

    /**
     * Test hook: replaces max_log_size_mb and EERIE_LEAP_LOG_MIN_FREE_SPACE_MB with byte counts
     * (0 disables either), so a small test volume can rotate and run short of space. Takes effect
     * at the next Start().
     */
    void SetSizeLimitsForTesting(uint64_t max_log_size_bytes, uint64_t min_free_space_bytes);
};

} // namespace eerie_leap::domain::logging_domain::services
