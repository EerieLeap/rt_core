#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <string>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "domain/logging_domain/loggers/i_logger.h"
#include "domain/sensor_domain/models/sensor_limits.h"
#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "subsys/canbus/can_frame.h"
#include "subsys/fs/services/fs_service_stream_buf.h"
#include "subsys/time/i_time_service.h"

#include "log_writer_stats.h"

namespace eerie_leap::domain::logging_domain::services {

using eerie_leap::subsys::fs::services::FsServiceStreamBuf;

using eerie_leap::domain::logging_domain::loggers::ILogger;
using eerie_leap::domain::sensor_domain::models::SensorLimits;
using eerie_leap::domain::sensor_domain::models::SensorReading;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;
using eerie_leap::subsys::canbus::CanFrame;
using eerie_leap::subsys::time::ITimeService;

struct LogWriterTask {
    std::shared_ptr<ITimeService> time_service;
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame;
    std::shared_ptr<ILogger<SensorReading>> logger;
    FsServiceStreamBuf* stream_buf = nullptr; // Owned by the service; valid while the task is scheduled.

    // Cleared by Stop() before it cancels, so a tick that is running then schedules no other.
    atomic_t is_running = ATOMIC_INIT(0);

    // Absolute deadlines in system ticks, so neither the tick's own duration nor the kernel's
    // rounding of a relative timeout adds to the period.
    int64_t interval_ticks = 0;
    int64_t burst_interval_ticks = 0; // Next deadline after a drain that filled the raw buffers.
    int64_t next_deadline = 0;

    // 0 never rotates. Set at the end of the tick that reached it, acted on at the start of the next.
    uint64_t max_log_size_bytes = 0;
    bool is_rotation_due = false;
    // Where the next file of a rotation starts: the time of the last tick rather than the rotation's,
    // so the frames and samples that arrived since are not older than the file and keep their times.
    std::chrono::system_clock::time_point last_tick_time;

    LogWriterStats stats;
    // Totals of the files a rotation closed; the stats add the open file to them.
    uint64_t closed_files_bytes = 0;
    uint32_t closed_files_stream_writes = 0;

    // The frame's drop count when last read; it starts again from 0 when processing restarts.
    uint32_t last_dropped_count = 0;
    uint32_t dropped_at_last_warning = 0;
    int64_t last_drop_warning_ms = 0;
    int64_t last_stats_log_ms = 0;

    // Sync cadence: 0 syncs only at finalize.
    int64_t sync_interval_ms = 0;
    int64_t last_sync_ms = 0;

    // Snapshot buffers, so a logging tick does not allocate.
    std::array<SensorReading, SensorLimits::MAX_COUNT> readings;
    std::array<SensorReading, SensorLimits::RAW_CAN_QUEUE_SIZE> raw_readings;
    std::array<CanFrame, SensorLimits::RAW_CAN_QUEUE_SIZE> raw_frames;
};

} // namespace eerie_leap::domain::logging_domain::services
