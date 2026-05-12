#pragma once

#include <memory>
// #include <atomic>
#include <chrono>
#include <vector>
#include <unordered_map>

#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>

#include "subsys/canbus/can_frame.h"
#include "subsys/threading/work_queue_thread.h"
#include "subsys/canbus/canbus_proxy.hpp"

#include "isr_sensor_reader_base.h"

namespace eerie_leap::domain::sensor_domain::isr_sensor_readers {

using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::subsys::canbus::CanFrame;
using eerie_leap::subsys::canbus::CanbusProxy;
using eerie_leap::domain::sensor_domain::models::SensorReading;

class CanbusSensorReaderRaw : public IsrSensorReaderBase {
private:
    std::shared_ptr<WorkQueueThread> work_queue_thread_;
    std::shared_ptr<CanbusProxy> canbus_;

    atomic_t is_destroying_{ATOMIC_INIT(0)};
    k_sem processing_semaphore_;
    uint32_t frame_id_;
    int frame_handler_id_;

    static constexpr int FRAME_PROCESSING_DELAY_MS = 4;

protected:
    std::optional<SensorReading> CreateRawReading(const CanFrame& can_frame);
    virtual void AddOrUpdateReading(const CanFrame can_frame);

public:
    CanbusSensorReaderRaw(
        std::shared_ptr<ITimeService> time_service,
        std::shared_ptr<GuidGenerator> guid_generator,
        std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
        std::shared_ptr<Sensor> sensor,
        ProcessSensorCallback process_sensor_callback,
        std::shared_ptr<WorkQueueThread> work_queue_thread,
        std::shared_ptr<CanbusProxy> canbus);
    virtual ~CanbusSensorReaderRaw();
};

} // namespace eerie_leap::domain::sensor_domain::isr_sensor_readers
