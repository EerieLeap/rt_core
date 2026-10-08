#pragma once

#include <array>
#include <memory>
#include <string>
#include <chrono>

#include <zephyr/kernel.h>

#include "subsys/canbus/can_frame.h"
#include "subsys/time/i_time_service.h"
#include "domain/sensor_domain/models/sensor_limits.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/logging_domain/loggers/i_logger.h"

namespace eerie_leap::domain::logging_domain::services {

using eerie_leap::subsys::canbus::CanFrame;
using eerie_leap::subsys::time::ITimeService;
using eerie_leap::domain::sensor_domain::models::SensorLimits;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;
using eerie_leap::domain::sensor_domain::models::SensorReading;
using eerie_leap::domain::logging_domain::loggers::ILogger;

struct LogWriterTask {
    k_timeout_t logging_interval_ms;
    std::shared_ptr<ITimeService> time_service;
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame;
    std::shared_ptr<ILogger<SensorReading>> logger;
    std::chrono::system_clock::time_point start_time;

    // Snapshot buffers, so a logging tick does not allocate.
    std::array<SensorReading, SensorLimits::kMaxCount> readings;
    std::array<SensorReading, SensorLimits::kRawCanQueueSize> raw_readings;
    std::array<CanFrame, SensorLimits::kRawCanQueueSize> raw_frames;
};

} // namespace eerie_leap::domain::logging_domain::services
