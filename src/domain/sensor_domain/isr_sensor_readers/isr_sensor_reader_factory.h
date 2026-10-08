#pragma once

#include <memory>

#include "subsys/time/i_time_service.h"
#include "subsys/threading/work_queue_thread.h"
#include "subsys/gpio/i_gpio.h"

#include "domain/sensor_domain/runtime/sensor_runtime.h"
#include "i_isr_sensor_reader.h"

namespace eerie_leap::domain::sensor_domain::isr_sensor_readers {

using eerie_leap::subsys::time::ITimeService;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::subsys::gpio::IGpio;

// Creates the event-driven reader for a sensor's source kind; null when the unit lacks that source.
// CAN sensors are fed by CanFrameRouter, not by a reader per sensor.
class IsrSensorReaderFactory {
protected:
    std::shared_ptr<ITimeService> time_service_;
    std::shared_ptr<IGpio> gpio_;

public:
    IsrSensorReaderFactory(
        std::shared_ptr<ITimeService> time_service,
        std::shared_ptr<IGpio> gpio);

    virtual ~IsrSensorReaderFactory() = default;

    std::unique_ptr<IIsrSensorReader> Create(
        const SensorRuntime& runtime,
        std::shared_ptr<WorkQueueThread> work_queue_thread,
        ProcessSensorCallback process_sensor_callback);
};

} // namespace eerie_leap::domain::sensor_domain::isr_sensor_readers
