#pragma once

#include <memory>
#include <optional>

#include "subsys/time/i_time_service.h"
#include "subsys/threading/work_queue_thread.h"
#include "subsys/gpio/i_gpio.h"
#include "subsys/canbus/can_id.h"

#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/runtime/sensor_runtime.h"
#include "domain/canbus_domain/services/canbus_service.h"
#include "i_isr_sensor_reader.h"

namespace eerie_leap::domain::sensor_domain::isr_sensor_readers {

using eerie_leap::subsys::time::ITimeService;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::subsys::gpio::IGpio;
using eerie_leap::subsys::canbus::CanId;

using eerie_leap::domain::sensor_domain::models::Sensor;
using eerie_leap::domain::canbus_domain::services::CanbusService;

// Creates the event-driven reader for a sensor's source kind; null when the unit lacks that source.
class IsrSensorReaderFactory {
protected:
    std::shared_ptr<ITimeService> time_service_;
    std::shared_ptr<CanbusService> canbus_service_;
    std::shared_ptr<IGpio> gpio_;

    [[nodiscard]] std::optional<CanId> GetFrameId(const Sensor& sensor) const;

public:
    IsrSensorReaderFactory(
        std::shared_ptr<ITimeService> time_service,
        std::shared_ptr<CanbusService> canbus_service,
        std::shared_ptr<IGpio> gpio);

    virtual ~IsrSensorReaderFactory() = default;

    std::unique_ptr<IIsrSensorReader> Create(
        const SensorRuntime& runtime,
        std::shared_ptr<WorkQueueThread> work_queue_thread,
        ProcessSensorCallback process_sensor_callback);
};

} // namespace eerie_leap::domain::sensor_domain::isr_sensor_readers
