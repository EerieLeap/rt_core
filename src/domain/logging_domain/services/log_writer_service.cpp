#include <algorithm>
#include <cstdio>
#include <exception>
#include <string>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "domain/logging_domain/event_bus/logging_events_channel.h"
#include "subsys/random/rng.h"
#include "subsys/threading/scoped_mutex.h"
#include "utilities/string/path_helpers.h"

#include "log_file_naming.h"
#include "log_writer_service.h"

// Deadlines are absolute uptime ticks, which the kernel only takes with 64-bit timeouts.
#ifndef CONFIG_TIMEOUT_64BIT
    #error "The log writer needs CONFIG_TIMEOUT_64BIT for absolute deadlines."
#endif

namespace eerie_leap::domain::logging_domain::services {

using namespace eerie_leap::subsys::random;
using namespace eerie_leap::utilities::string;
using eerie_leap::domain::logging_domain::event_bus::LoggingEventsChannel;
using eerie_leap::domain::logging_domain::event_bus::LoggingEventType;
using eerie_leap::domain::logging_domain::event_bus::LoggingPayloadType;
using eerie_leap::subsys::threading::ScopedMutex;

LOG_MODULE_REGISTER(log_writer_service_logger);

// After a drain that filled the raw buffers the queue may be dropping frames already, so the next
// tick comes at a quarter of the interval, but not sooner than this.
static constexpr int64_t MIN_BURST_INTERVAL_MS = 10;
static constexpr int64_t DROP_WARNING_PERIOD_MS = static_cast<int64_t>(CONFIG_EERIE_LEAP_LOG_STATS_LOG_PERIOD_S) * 1000;

LogWriterService::LogWriterService(
    std::shared_ptr<IFsService> fs_service,
    std::shared_ptr<LoggingConfigurationManager> logging_configuration_manager,
    std::shared_ptr<ITimeService> time_service,
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame
) :
    work_queue_thread_(nullptr),
    work_queue_task_(std::nullopt),
    fs_service_(std::move(fs_service)),
    logging_configuration_manager_(std::move(logging_configuration_manager)),
    time_service_(std::move(time_service)),
    sensor_readings_frame_(std::move(sensor_readings_frame)) {

    k_mutex_init(&open_file_mutex_);
}

void LogWriterService::SetLoggerFactory(LoggerFactory logger_factory) {
    logger_factory_ = std::move(logger_factory);
}

void LogWriterService::SetSizeLimitsForTesting(uint64_t max_log_size_bytes, uint64_t min_free_space_bytes) {
    max_log_size_bytes_override_ = max_log_size_bytes;
    min_free_space_bytes_ = min_free_space_bytes;
}

bool LogWriterService::DoInitialize() {
    work_queue_thread_ = std::make_unique<WorkQueueThread>("log_writer_service", THREAD_STACK_SIZE, THREAD_PRIORITY);

    if(!work_queue_thread_->Initialize()) {
        LOG_ERR("Failed to initialize the log writer work queue.");
        return false;
    }

    auto task = std::make_unique<LogWriterTask>();
    task->time_service = time_service_;
    task->sensor_readings_frame = sensor_readings_frame_;

    work_queue_task_ = work_queue_thread_->CreateTask(
        [this](LogWriterTask* writer_task) {
            return ProcessTick(*writer_task);
        },
        std::move(task)
    );

    LOG_INF("Log writer service initialized.");

    return true;
}

bool LogWriterService::DoStart() {
    if(!logger_factory_) {
        LOG_ERR("No logger factory is set.");
        return false;
    }

    if(!fs_service_->IsAvailable()) {
        LOG_ERR("SD card is not available.");
        return false;
    }

    if(!fs_service_->Exists(LogFileNaming::LOGS_DIR) && !fs_service_->CreateDirectory(LogFileNaming::LOGS_DIR)) {
        LOG_ERR("Failed to create %s directory", LogFileNaming::LOGS_DIR);
        return false;
    }

    const auto logging_configuration = logging_configuration_manager_->Get();
    if(logging_configuration == nullptr) {
        LOG_ERR("No logging configuration is available.");
        return false;
    }

    if(!HasMinimumFreeSpace())
        return false;

    try {
        logger_ = logger_factory_();
    } catch(const std::exception& e) {
        LOG_ERR("Failed to create the logger: %s", e.what());
        logger_ = nullptr;
    }

    if(logger_ == nullptr) {
        LOG_ERR("Logger is not available.");
        return false;
    }

    if(!work_queue_task_.has_value()) {
        LOG_ERR("The log writer service is not initialized.");
        return false;
    }

    auto& work_queue_task = *work_queue_task_;

    // The frame queues raw CAN frames whether or not logging runs. Whatever it holds now predates
    // this file and would, at the first tick, crowd out the frames that arrive after the start.
    auto& task = *work_queue_task.GetUserdata();
    while(sensor_readings_frame_->TakeRawCanFrames(task.raw_readings, task.raw_frames) > 0) {
    }

    if(!OpenLogFile(time_service_->GetCurrentTime()))
        return false;

    LOG_INF("Logging started. Log file created: %s", file_path_.CStr());

    const int64_t interval_ms = logging_configuration->logging_interval_ms;
    task.logger = logger_;
    task.stream_buf = fs_stream_buf_.get();
    task.interval_ticks = static_cast<int64_t>(k_ms_to_ticks_ceil64(static_cast<uint64_t>(interval_ms)));
    task.burst_interval_ticks = static_cast<int64_t>(
        k_ms_to_ticks_ceil64(static_cast<uint64_t>(std::max(interval_ms / 4, MIN_BURST_INTERVAL_MS)))
    );
    task.max_log_size_bytes = max_log_size_bytes_override_.value_or(
        static_cast<uint64_t>(logging_configuration->max_log_size_mb) * 1024 * 1024
    );
    task.is_rotation_due = false;
    task.stats = LogWriterStats{};
    task.closed_files_bytes = 0;
    task.closed_files_stream_writes = 0;
    task.last_dropped_count = sensor_readings_frame_->GetDroppedRawCanFrameCount();
    task.dropped_at_last_warning = 0;

    const int64_t now_ms = k_uptime_get();
    task.last_drop_warning_ms = now_ms - DROP_WARNING_PERIOD_MS;
    task.last_stats_log_ms = now_ms;
    task.sync_interval_ms = CONFIG_EERIE_LEAP_LOG_SYNC_INTERVAL_MS;
    task.last_sync_ms = now_ms;

    has_failed_ = false;
    atomic_set(&task.is_running, 1);

    // Before the first tick is armed, so a failure of that tick is never published ahead of it.
    PublishStatus(true);

    task.next_deadline = k_uptime_ticks() + task.interval_ticks;
    work_queue_task.Reschedule(K_TIMEOUT_ABS_TICKS(task.next_deadline));

    return true;
}

bool LogWriterService::DoStop() {
    // Never initialized, so never started.
    if(!work_queue_task_.has_value())
        return true;

    auto& work_queue_task = *work_queue_task_;
    auto& task = *work_queue_task.GetUserdata();

    atomic_clear(&task.is_running);

    // Cancel() waits for a tick in progress, which, seeing the flag, schedules no other; the loop
    // takes a reschedule that raced the flag.
    while(work_queue_task.Cancel())
        k_sleep(K_MSEC(1));

    CloseLogFile(task);

    LOG_INF("Logging stopped.");

    PublishStatus(false, LoggingStatusReason::Stopped);

    return true;
}

LogWriterStats LogWriterService::GetStats() const {
    if(!work_queue_task_.has_value())
        return {};

    return work_queue_task_.value().GetUserdata()->stats;
}

void LogWriterService::PublishStatus(bool is_active, LoggingStatusReason reason) {
    LoggingEventsChannel::EventMessage event{ .type = LoggingEventType::StatusUpdated };

    event.payload.insert_or_assign(LoggingPayloadType::IsActive, is_active);
    if(!is_active)
        event.payload.insert_or_assign(LoggingPayloadType::Reason, static_cast<uint32_t>(reason));

    // Queued for the bus thread, so no subscriber runs on the writer thread or inside a transition
    // of this service, where a Stop() would wait for itself.
    LoggingEventsChannel::GetInstance().PublishAsync(event);
}

bool LogWriterService::HasMinimumFreeSpace() const {
    if(min_free_space_bytes_ == 0)
        return true;

    // Both read the free cluster count, for which FatFs may scan the whole FAT of a large card.
    const uint64_t total_bytes = fs_service_->GetTotalSpace();
    const uint64_t used_bytes = fs_service_->GetUsedSpace();
    const uint64_t free_bytes = total_bytes > used_bytes ? total_bytes - used_bytes : 0;

    if(free_bytes >= min_free_space_bytes_)
        return true;

    LOG_ERR(
        "%llu bytes free on the log volume, %llu needed.", static_cast<unsigned long long>(free_bytes),
        static_cast<unsigned long long>(min_free_space_bytes_)
    );

    return false;
}

bool LogWriterService::OpenLogFile(const std::chrono::system_clock::time_point& start_time) {
    file_path_.Clear();

    for(int i = 0; i < 10; i++) {
        const auto new_file_path = LogFileNaming::GetFilePath(
            LogFileNaming::LOGS_DIR, CONFIG_EERIE_LEAP_LOG_DATA_FILE_PREFIX, start_time, Rng::Get<uint16_t>(),
            logger_->GetFileExtension()
        );

        if(!fs_service_->Exists(new_file_path.ToString())) {
            file_path_ = new_file_path;
            break;
        }
    }

    if(file_path_.Empty()) {
        LOG_ERR("Failed to create log file name");
        return false;
    }

    // Before the file exists, so that no moment of it goes unreported; every failure below clears it.
    SetOpenFile(file_path_.ToString());

    try {
        // Write mode keeps the stream seekable, which the MDF4 logger needs to finalize the file.
        fs_stream_buf_ = std::make_unique<FsServiceStreamBuf>(
            fs_service_.get(), file_path_.ToString(), FsServiceStreamBuf::OpenMode::Write
        );
    } catch(const std::exception& e) {
        LOG_ERR("Failed to open the log file %s: %s", file_path_.CStr(), e.what());
        fs_stream_buf_.reset();
        ClearOpenFile();
        return false;
    }

    bool is_started = false;
    try {
        is_started = logger_->StartLogging(*fs_stream_buf_, start_time);
    } catch(const std::exception& e) {
        LOG_ERR("%s", e.what());
    }

    if(!is_started) {
        LOG_ERR("Failed to write the log file header: %s", file_path_.CStr());

        fs_stream_buf_->close();
        fs_stream_buf_.reset();
        fs_service_->DeleteFile(file_path_.ToString());
        ClearOpenFile();

        return false;
    }

    return true;
}

bool LogWriterService::CloseLogFile(LogWriterTask& task) {
    // The stream goes now, so the task must not look at it any more.
    task.stream_buf = nullptr;

    if(fs_stream_buf_ == nullptr) {
        ClearOpenFile();

        return false;
    }

    // Taken before finalizing, which rewrites blocks in place.
    task.closed_files_bytes += logger_->GetBytesWritten();

    const bool is_finalized = logger_->StopLogging();
    if(!is_finalized)
        LOG_ERR("Failed to finalize %s; it stays readable as unfinalized.", file_path_.CStr());

    if(!fs_stream_buf_->close())
        LOG_ERR("Failed to write the end of %s before closing it.", file_path_.CStr());

    task.closed_files_stream_writes += fs_stream_buf_->GetWriteCallCount();
    fs_stream_buf_.reset();

    // Once the file is closed and final, not while it is being finalized: a reader takes it from here.
    ClearOpenFile();

    return is_finalized;
}

void LogWriterService::SetOpenFile(std::string_view path) {
    ScopedMutex lock(open_file_mutex_);

    open_file_path_ = StaticString<LogFileNaming::PATH_MAX_LENGTH>(path);
}

void LogWriterService::ClearOpenFile() {
    ScopedMutex lock(open_file_mutex_);

    open_file_path_.Clear();
}

bool LogWriterService::IsOpenFile(std::string_view path) const {
    ScopedMutex lock(open_file_mutex_);

    return PathHelpers::IsSameEntryIgnoringCase(path, open_file_path_.ToString());
}

std::optional<LoggingStatusReason> LogWriterService::RotateFile(LogWriterTask& task) {
    // Checked while the full file is still open, so the stop that follows finalizes it.
    if(!HasMinimumFreeSpace())
        return LoggingStatusReason::NoSpace;

    if(!CloseLogFile(task))
        task.stats.write_errors++;

    if(!OpenLogFile(task.last_tick_time)) {
        task.stats.write_errors++;
        return LoggingStatusReason::RotationFailed;
    }

    task.stream_buf = fs_stream_buf_.get();
    task.is_rotation_due = false;
    // StartLogging() synced the header.
    task.last_sync_ms = k_uptime_get();
    task.stats.rotations++;

    LOG_INF("Log file rotated. Log file created: %s", file_path_.CStr());

    return std::nullopt;
}

WorkQueueTaskResult LogWriterService::EndSession(LoggingStatusReason reason) {
    has_failed_ = true;

    // Stop() would wait for this very tick; the supervisor stops the service from another thread
    // when the event arrives, which finalizes what it can and closes the file.
    PublishStatus(false, reason);

    return { .reschedule = false };
}

WorkQueueTaskResult LogWriterService::ProcessTick(LogWriterTask& task) {
    if(atomic_get(&task.is_running) == 0)
        return { .reschedule = false };

    const uint32_t tick_start_cycles = k_cycle_get_32();
    bool is_drain_full = false;

    try {
        // Before the snapshot, so every reading of this tick goes into the new file.
        if(task.is_rotation_due) {
            const auto failure = RotateFile(task);
            if(failure.has_value()) {
                LOG_ERR("Failed to rotate the log file; logging stops.");
                return EndSession(failure.value());
            }
        }

        const auto time_now = task.time_service->GetCurrentTime();
        task.last_tick_time = time_now;

        // Values are sampled at the logging interval; raw CAN frames come through their queue below,
        // so every frame is logged, not only the latest one. The logger writes each reading at its own
        // timestamp; the tick time is for a reading without one and for a held sample written again.
        // A CANBUS_RAW sensor's latest reading carries no frame, so the logger skips it here; the
        // tick never follows a reading's sensor pointer.
        const size_t count = task.sensor_readings_frame->SnapshotLoggableReadings(task.readings);
        for(size_t i = 0; i < count; i++) {
            if(task.logger->LogReading(time_now, task.readings[i]))
                task.stats.readings++;
        }

        size_t raw_count = 0;
        do {
            raw_count = task.sensor_readings_frame->TakeRawCanFrames(task.raw_readings, task.raw_frames);
            is_drain_full = is_drain_full || raw_count == task.raw_readings.size();

            for(size_t i = 0; i < raw_count; i++) {
                if(task.logger->LogReading(time_now, task.raw_readings[i]))
                    task.stats.frames++;
            }
        } while(raw_count == task.raw_readings.size());

        // A sync puts the directory entry and the allocation on the card, so a power loss
        // costs at most one interval plus one stream buffer of records.
        if(task.sync_interval_ms > 0) {
            const int64_t sync_now_ms = k_uptime_get();

            if(sync_now_ms - task.last_sync_ms >= task.sync_interval_ms) {
                task.last_sync_ms = sync_now_ms;

                // The stream refuses every write after one that failed or fell short, so the
                // session cannot go on.
                if(task.stream_buf->pubsync() != 0) {
                    task.stats.write_errors++;
                    LOG_ERR("Failed to sync the log file; logging stops.");
                    return EndSession(LoggingStatusReason::WriteFailed);
                }

                task.stats.syncs++;
            }
        }

        task.is_rotation_due = task.max_log_size_bytes > 0 && task.logger->GetBytesWritten() >= task.max_log_size_bytes;
    } catch(const std::exception& e) {
        task.stats.write_errors++;
        LOG_ERR("Failed to write the log file; logging stops. %s", e.what());
        return EndSession(LoggingStatusReason::WriteFailed);
    } catch(...) {
        task.stats.write_errors++;
        LOG_ERR("Failed to write the log file; logging stops.");
        return EndSession(LoggingStatusReason::WriteFailed);
    }

    task.stats.ticks++;

    // The frame counts from 0 again when processing restarts under a running writer (a CAN
    // reconfiguration restarts it without the writer).
    const uint32_t dropped_count = task.sensor_readings_frame->GetDroppedRawCanFrameCount();
    task.stats.frames_dropped += dropped_count >= task.last_dropped_count ? dropped_count - task.last_dropped_count
                                                                          : dropped_count;
    task.last_dropped_count = dropped_count;

    task.stats.bytes = task.closed_files_bytes + task.logger->GetBytesWritten();
    task.stats.stream_writes = task.closed_files_stream_writes + task.stream_buf->GetWriteCallCount();

    const uint32_t tick_us = k_cyc_to_us_near32(k_cycle_get_32() - tick_start_cycles);
    task.stats.tick_last_us = tick_us;
    task.stats.tick_max_us = std::max(task.stats.tick_max_us, tick_us);

    const int64_t now_ms = k_uptime_get();
    if(task.stats.frames_dropped > task.dropped_at_last_warning
        && now_ms - task.last_drop_warning_ms >= DROP_WARNING_PERIOD_MS) {

        LOG_WRN(
            "%u raw CAN frames dropped before the log writer drained them (%u since the start); "
            "EERIE_LEAP_DOMAIN_SENSOR_RAW_CAN_QUEUE_SIZE is too small for the frame rate.",
            task.stats.frames_dropped - task.dropped_at_last_warning, task.stats.frames_dropped
        );

        task.dropped_at_last_warning = task.stats.frames_dropped;
        task.last_drop_warning_ms = now_ms;
    }

#ifdef CONFIG_EERIE_LEAP_DOMAIN_LOGGING_STATS
    if(now_ms - task.last_stats_log_ms >= CONFIG_EERIE_LEAP_LOG_STATS_LOG_PERIOD_S * 1000) {
        task.last_stats_log_ms = now_ms;
        const auto& stats = task.stats;
        LOG_INF(
            "Log writer: ticks %u, readings %u, frames %u (dropped %u), bytes %llu, writes %u, syncs %u, rotations %u, "
            "overruns %u, errors %u, tick last/max %u/%u us",
            stats.ticks, stats.readings, stats.frames, stats.frames_dropped,
            static_cast<unsigned long long>(stats.bytes), stats.stream_writes, stats.syncs, stats.rotations,
            stats.overruns, stats.write_errors, stats.tick_last_us, stats.tick_max_us
        );
    }
#endif

    // A tick that ran past the next deadline keeps the phase: the missed periods are skipped and
    // counted rather than run late, back to back.
    const int64_t now_ticks = k_uptime_ticks();
    task.next_deadline += task.interval_ticks;
    if(task.next_deadline <= now_ticks) {
        const int64_t missed = (now_ticks - task.next_deadline) / task.interval_ticks + 1;
        task.next_deadline += missed * task.interval_ticks;
        task.stats.overruns += static_cast<uint32_t>(missed);
    }

    // A drain that filled the buffers may have left frames behind: come back sooner than the phase.
    if(is_drain_full)
        task.next_deadline = std::min(task.next_deadline, now_ticks + task.burst_interval_ticks);

    // Stop() started while the tick ran: leave nothing behind for its cancel to chase.
    if(atomic_get(&task.is_running) == 0)
        return { .reschedule = false };

    return { .reschedule = true, .delay = K_TIMEOUT_ABS_TICKS(task.next_deadline) };
}

} // namespace eerie_leap::domain::logging_domain::services
