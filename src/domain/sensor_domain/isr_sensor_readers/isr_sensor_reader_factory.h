#pragma once

#include <memory>

#include "utilities/guid/guid_generator.h"
#include "subsys/time/i_time_service.h"
#include "subsys/threading/work_queue_thread.h"
#include "subsys/gpio/i_gpio.h"

#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/canbus_domain/services/canbus_service.h"
#include "i_isr_sensor_reader.h"

namespace eerie_leap::domain::sensor_domain::isr_sensor_readers {

using eerie_leap::utilities::guid::GuidGenerator;
using eerie_leap::subsys::time::ITimeService;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::subsys::gpio::IGpio;

using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;
using eerie_leap::domain::canbus_domain::services::CanbusService;

class IsrSensorReaderFactory {
protected:
    std::shared_ptr<ITimeService> time_service_;
    std::shared_ptr<GuidGenerator> guid_generator_;
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame_;
    std::shared_ptr<CanbusService> canbus_service_;
    std::shared_ptr<IGpio> gpio_;

public:
    IsrSensorReaderFactory(
        std::shared_ptr<ITimeService> time_service,
        std::shared_ptr<GuidGenerator> guid_generator,
        std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
        std::shared_ptr<CanbusService> canbus_service,
        std::shared_ptr<IGpio> gpio);

    virtual ~IsrSensorReaderFactory() = default;

    std::unique_ptr<IIsrSensorReader> Create(
        std::shared_ptr<Sensor> sensor,
        std::shared_ptr<WorkQueueThread> work_queue_thread,
        ProcessSensorCallback process_sensor_callback);
};

} // namespace eerie_leap::domain::sensor_domain::isr_sensor_readers
