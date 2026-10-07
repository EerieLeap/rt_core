#pragma once

#include <memory>

#include "subsys/time/i_time_service.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/models/reading_source.h"
#include "i_sensor_reader.h"

namespace eerie_leap::domain::sensor_domain::sensor_readers {

using eerie_leap::subsys::time::ITimeService;
using eerie_leap::domain::sensor_domain::models::Sensor;
using eerie_leap::domain::sensor_domain::models::ReadingSource;

class SensorReaderBase : public ISensorReader {
protected:
    std::shared_ptr<ITimeService> time_service_;
    std::shared_ptr<Sensor> sensor_;

    SensorReading CreateReading() const {
        SensorReading reading(sensor_.get());
        reading.source = ReadingSource::PROCESSING;
        reading.timestamp = time_service_->GetCurrentTime();

        return reading;
    }

public:
    SensorReaderBase(std::shared_ptr<ITimeService> time_service, std::shared_ptr<Sensor> sensor)
        : time_service_(std::move(time_service)), sensor_(std::move(sensor)) {}

    virtual ~SensorReaderBase() = default;
};

} // namespace eerie_leap::domain::sensor_domain::sensor_readers
