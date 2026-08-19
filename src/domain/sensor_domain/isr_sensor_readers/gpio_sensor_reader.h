#pragma once

#include <memory>

#include <zephyr/kernel.h>

#include "subsys/gpio/i_gpio.h"
#include "subsys/threading/work_queue_thread.h"

#include "isr_dispatch_guard.hpp"
#include "isr_sensor_reader_base.h"

namespace eerie_leap::domain::sensor_domain::isr_sensor_readers {

using eerie_leap::subsys::gpio::IGpio;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::domain::sensor_domain::models::SensorReading;

class GpioSensorReader : public IsrSensorReaderBase {
private:
    std::shared_ptr<WorkQueueThread> work_queue_thread_;
    std::shared_ptr<IGpio> gpio_;
    std::shared_ptr<IsrDispatchGuard<GpioSensorReader>> dispatch_guard_;

    int channel_ = 0;
    int handler_id_ = 0;

    static constexpr int STATE_PROCESSING_DELAY_MS = 4;

    void QueueReading(bool state);
    void ProcessState(bool state) noexcept;
    void AddOrUpdateReading(bool state);

public:
    GpioSensorReader(
        std::shared_ptr<ITimeService> time_service,
        std::shared_ptr<GuidGenerator> guid_generator,
        std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
        std::shared_ptr<Sensor> sensor,
        ProcessSensorCallback process_sensor_callback,
        std::shared_ptr<WorkQueueThread> work_queue_thread,
        std::shared_ptr<IGpio> gpio);
    ~GpioSensorReader() override;
};

} // namespace eerie_leap::domain::sensor_domain::isr_sensor_readers
