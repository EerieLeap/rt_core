#pragma once

#include <memory>

#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>

#include "subsys/canbus/can_frame.h"
#include "subsys/threading/work_queue_thread.h"
#include "subsys/canbus/canbus_proxy.hpp"

#include "isr_sensor_reader_base.h"

namespace eerie_leap::domain::sensor_domain::isr_sensor_readers {

using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::subsys::canbus::CanFrame;
using eerie_leap::subsys::canbus::CanId;
using eerie_leap::subsys::canbus::CanbusProxy;

// Receives the frames of one CAN ID on the CAN thread and processes them on the work queue.
//
// The hand-off is a single pending slot: the CAN thread copies the newest frame into it and
// submits the embedded work item, which is a no-op while one is already queued. Frames arriving
// faster than the work queue drains are coalesced, newest wins, and nothing is allocated per frame.
class CanbusSensorReaderRaw : public IsrSensorReaderBase {
private:
    struct PendingWork {
        k_work work;
        CanbusSensorReaderRaw* reader;
    };

    std::shared_ptr<WorkQueueThread> work_queue_thread_;
    std::shared_ptr<CanbusProxy> canbus_;

    int frame_handler_id_ = 0;

    PendingWork pending_work_{};
    k_work_sync work_sync_{};
    k_spinlock pending_lock_{};
    CanFrame pending_frame_{};
    bool has_pending_frame_ = false;

    static void WorkHandler(k_work* work);

    void OnFrameReceived(const CanFrame& can_frame);
    void ProcessPendingFrame() noexcept;

protected:
    // Fills the reading from the frame. The raw reader keeps the frame; decoders set the value.
    virtual void FillReading(SensorReading& reading, const CanFrame& can_frame);

    // Derived readers must call this from their destructor so no frame is
    // dispatched onto their already destroyed members.
    void Detach();

public:
    CanbusSensorReaderRaw(
        std::shared_ptr<ITimeService> time_service,
        const SensorRuntime& runtime,
        ProcessSensorCallback process_sensor_callback,
        std::shared_ptr<WorkQueueThread> work_queue_thread,
        std::shared_ptr<CanbusProxy> canbus,
        const CanId& frame_id);
    virtual ~CanbusSensorReaderRaw();
};

} // namespace eerie_leap::domain::sensor_domain::isr_sensor_readers
