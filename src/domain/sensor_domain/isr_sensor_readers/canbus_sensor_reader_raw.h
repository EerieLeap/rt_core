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

#include "isr_dispatch_guard.hpp"
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
    std::shared_ptr<IsrDispatchGuard<CanbusSensorReaderRaw>> dispatch_guard_;

    uint32_t frame_id_ = 0;
    int frame_handler_id_ = 0;

    static constexpr int FRAME_PROCESSING_DELAY_MS = 4;

    void ProcessFrame(const CanFrame& can_frame) noexcept;

protected:
    std::optional<SensorReading> CreateRawReading(const CanFrame& can_frame);
    virtual void AddOrUpdateReading(const CanFrame& can_frame);

    // Derived readers must call this from their destructor so no frame is
    // dispatched onto their already destroyed members.
    void Detach();

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
