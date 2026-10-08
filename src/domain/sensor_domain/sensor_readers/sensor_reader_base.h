#pragma once

#include <memory>

#include "subsys/time/i_time_service.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/models/reading_source.h"
#include "domain/sensor_domain/runtime/sensor_runtime.h"
#include "i_sensor_reader.h"

namespace eerie_leap::domain::sensor_domain::sensor_readers {

using eerie_leap::subsys::time::ITimeService;
using eerie_leap::domain::sensor_domain::models::Sensor;
using eerie_leap::domain::sensor_domain::models::ReadingSource;
using eerie_leap::domain::sensor_domain::runtime::SensorRuntime;

// Readers refer to the runtime of their generation, which outlives them.
class SensorReaderBase : public ISensorReader {
protected:
    std::shared_ptr<ITimeService> time_service_;
    const SensorRuntime* runtime_;

    [[nodiscard]] const Sensor& GetSensor() const { return runtime_->GetSensor(); }

    SensorReading CreateReading() const {
        SensorReading reading(runtime_->sensor.get());
        reading.source = ReadingSource::PROCESSING;
        reading.timestamp = time_service_->GetCurrentTime();

        return reading;
    }

public:
    SensorReaderBase(std::shared_ptr<ITimeService> time_service, const SensorRuntime& runtime)
        : time_service_(std::move(time_service)), runtime_(&runtime) {}

    virtual ~SensorReaderBase() = default;
};

} // namespace eerie_leap::domain::sensor_domain::sensor_readers
