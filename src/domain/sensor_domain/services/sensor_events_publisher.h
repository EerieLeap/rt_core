#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

#include <zephyr/kernel.h>

#include "subsys/event_bus/acceptance_cache.h"
#include "subsys/threading/service_base.h"
#include "subsys/threading/work_queue_thread.h"
#include "domain/sensor_domain/event_bus/sensor_events_channel.h"
#include "domain/sensor_domain/models/sensor_limits.h"
#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"

namespace eerie_leap::domain::sensor_domain::services {

namespace threading = eerie_leap::subsys::threading;

using threading::ServiceBase;
using threading::WorkQueueThread;
using threading::WorkQueueTaskResult;
using eerie_leap::subsys::event_bus::AcceptanceCache;
using eerie_leap::domain::sensor_domain::event_bus::SensorEventsChannel;
using eerie_leap::domain::sensor_domain::models::SensorLimits;
using eerie_leap::domain::sensor_domain::models::SensorReading;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;

// Publishes what the frame processed as SensorEventType::DataUpdated {SensorId, Value} on
// sensor_events, from a work queue thread of its own every
// EERIE_LEAP_DOMAIN_SENSOR_EVENTS_PUBLISHER_PERIOD_MS. A tick takes every reading processed since
// the last one, so a unit runs one publisher, and publishes those with a value that a subscriber
// accepts. Acceptance is asked once per sensor and subscription change; a reading nobody accepts
// is taken and dropped. Built with EERIE_LEAP_EVENT_BUS.
class SensorEventsPublisher final : public ServiceBase<> {
private:
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame_;

    // Filled by each tick, so a tick does not allocate. Only the publisher thread touches them.
    std::array<SensorReading, SensorLimits::kMaxCount> readings_;
    AcceptanceCache<SensorLimits::kMaxCount> acceptance_cache_;
    std::optional<uint32_t> filter_error_generation_;   // The generation a filter error was logged in.
    bool is_tick_error_logged_ = false;

    // Declared after the thread, so the task goes first.
    std::unique_ptr<WorkQueueThread> work_queue_thread_;
    std::optional<threading::WorkQueueTask<SensorEventsPublisher>> work_queue_task_;

    static WorkQueueTaskResult ProcessWorkTask(SensorEventsPublisher* publisher);
    void PublishProcessedReadings();
    bool IsAccepted(
        const SensorEventsChannel& channel,
        const SensorEventsChannel::EventMessage& event,
        uint32_t sensor_id_hash,
        uint32_t subscription_generation);
    void LogFilterError(uint32_t sensor_id_hash, uint32_t subscription_generation, const char* error);
    void CancelTask();

    bool DoInitialize() override;
    bool DoStart() override;
    bool DoStop() override;
    bool DoPause() override;
    bool DoResume() override;

public:
    explicit SensorEventsPublisher(std::shared_ptr<SensorReadingsFrame> sensor_readings_frame);
    ~SensorEventsPublisher() override;

    SensorEventsPublisher(const SensorEventsPublisher&) = delete;
    SensorEventsPublisher& operator=(const SensorEventsPublisher&) = delete;

    [[nodiscard]] bool IsPausable() const noexcept override { return true; }

    // The next tick publishes the latest processed value of every accepted sensor once more, so a
    // subscriber that has just subscribed sees the current state. Safe from any thread; paused or
    // stopped, it takes effect once the publisher runs again.
    void Replay();
};

} // namespace eerie_leap::domain::sensor_domain::services
