#pragma once

#include <memory>

#include "utilities/memory/memory_resource_manager.h"
#include "utilities/guid/guid_generator.h"
#include "subsys/time/i_time_service.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "i_isr_sensor_reader.h"

namespace eerie_leap::domain::sensor_domain::isr_sensor_readers {

using eerie_leap::utilities::guid::GuidGenerator;
using eerie_leap::subsys::time::ITimeService;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;

class IsrSensorReaderBase : public IIsrSensorReader {
protected:
    std::shared_ptr<ITimeService> time_service_;
    std::shared_ptr<GuidGenerator> guid_generator_;
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame_;
    std::shared_ptr<Sensor> sensor_;
    ProcessSensorCallback process_sensor_callback_;

public:
    IsrSensorReaderBase(
        std::shared_ptr<ITimeService> time_service,
        std::shared_ptr<GuidGenerator> guid_generator,
        std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
        std::shared_ptr<Sensor> sensor,
        ProcessSensorCallback process_sensor_callback)
            : time_service_(std::move(time_service)),
            guid_generator_(std::move(guid_generator)),
            sensor_readings_frame_(std::move(sensor_readings_frame)),
            sensor_(std::move(sensor)),
            process_sensor_callback_(std::move(process_sensor_callback)) {}

    virtual ~IsrSensorReaderBase() = default;
};

} // namespace eerie_leap::domain::sensor_domain::isr_sensor_readers
