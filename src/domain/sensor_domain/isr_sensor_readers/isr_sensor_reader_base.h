#pragma once

#include <memory>

#include "subsys/time/i_time_service.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/models/reading_source.h"
#include "i_isr_sensor_reader.h"

namespace eerie_leap::domain::sensor_domain::isr_sensor_readers {

using eerie_leap::subsys::time::ITimeService;
using eerie_leap::domain::sensor_domain::models::ReadingSource;

class IsrSensorReaderBase : public IIsrSensorReader {
protected:
    std::shared_ptr<ITimeService> time_service_;
    std::shared_ptr<Sensor> sensor_;
    ProcessSensorCallback process_sensor_callback_;

    SensorReading CreateReading() const {
        SensorReading reading(sensor_.get());
        reading.source = ReadingSource::ISR;
        reading.timestamp = time_service_->GetCurrentTime();

        return reading;
    }

public:
    IsrSensorReaderBase(
        std::shared_ptr<ITimeService> time_service,
        std::shared_ptr<Sensor> sensor,
        ProcessSensorCallback process_sensor_callback)
            : time_service_(std::move(time_service)),
            sensor_(std::move(sensor)),
            process_sensor_callback_(std::move(process_sensor_callback)) {}

    virtual ~IsrSensorReaderBase() = default;
};

} // namespace eerie_leap::domain::sensor_domain::isr_sensor_readers
