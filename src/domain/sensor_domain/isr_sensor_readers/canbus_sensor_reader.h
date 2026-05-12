#pragma once

#include <memory>

#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>

#include "subsys/canbus/canbus_proxy.hpp"
#include "subsys/dbc/dbc.h"

#include "canbus_sensor_reader_raw.h"

namespace eerie_leap::domain::sensor_domain::isr_sensor_readers {

using eerie_leap::subsys::canbus::CanFrame;
using eerie_leap::subsys::canbus::CanbusProxy;
using eerie_leap::subsys::dbc::Dbc;

class CanbusSensorReader : public CanbusSensorReaderRaw {
private:
    std::shared_ptr<Dbc> dbc_;

    void AddOrUpdateReading(const CanFrame can_frame) override;

public:
    CanbusSensorReader(
        std::shared_ptr<ITimeService> time_service,
        std::shared_ptr<GuidGenerator> guid_generator,
        std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
        std::shared_ptr<Sensor> sensor,
        ProcessSensorCallback process_sensor_callback,
        std::shared_ptr<WorkQueueThread> work_queue_thread,
        std::shared_ptr<CanbusProxy> canbus,
        std::shared_ptr<Dbc> dbc);
    virtual ~CanbusSensorReader() = default;
};

} // namespace eerie_leap::domain::sensor_domain::isr_sensor_readers
