#pragma once

#include <memory>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include "subsys/gpio/i_gpio.h"
#include "subsys/threading/work_queue_thread.h"

#include "isr_sensor_reader_base.h"

namespace eerie_leap::domain::sensor_domain::isr_sensor_readers {

using eerie_leap::subsys::gpio::IGpio;
using eerie_leap::subsys::threading::WorkQueueThread;

// Reports the level of one GPIO channel on every edge, processed on the work queue.
// Like the CAN readers, edges arriving while one is queued are coalesced to the newest level.
class GpioSensorReader : public IsrSensorReaderBase {
private:
    struct PendingWork {
        k_work work;
        GpioSensorReader* reader;
    };

    std::shared_ptr<WorkQueueThread> work_queue_thread_;
    std::shared_ptr<IGpio> gpio_;

    int channel_ = 0;
    int handler_id_ = 0;

    PendingWork pending_work_{};
    k_work_sync work_sync_{};
    atomic_t pending_state_ = ATOMIC_INIT(0);

    static void WorkHandler(k_work* work);

    void QueueState(bool state);
    void ProcessPendingState() noexcept;

public:
    GpioSensorReader(
        std::shared_ptr<ITimeService> time_service,
        std::shared_ptr<Sensor> sensor,
        ProcessSensorCallback process_sensor_callback,
        std::shared_ptr<WorkQueueThread> work_queue_thread,
        std::shared_ptr<IGpio> gpio);
    ~GpioSensorReader() override;
};

} // namespace eerie_leap::domain::sensor_domain::isr_sensor_readers
