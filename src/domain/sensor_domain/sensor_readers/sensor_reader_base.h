#pragma once

#include <memory>

#include "domain/sensor_domain/models/reading_source.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/runtime/sensor_runtime.h"
#include "i_sensor_reader.h"
#include "subsys/time/i_time_service.h"

namespace eerie_leap::domain::sensor_domain::sensor_readers {

using eerie_leap::domain::sensor_domain::models::ReadingSource;
using eerie_leap::domain::sensor_domain::models::Sensor;
using eerie_leap::domain::sensor_domain::runtime::SensorRuntime;
using eerie_leap::subsys::time::ITimeService;

// Readers refer to the runtime of their generation, which outlives them.
class SensorReaderBase : public ISensorReader {
protected:
    std::shared_ptr<ITimeService> time_service_;
    const SensorRuntime* runtime_;

    [[nodiscard]] const Sensor& GetSensor() const {
        return runtime_->GetSensor();
    }

    SensorReading CreateReading(const TickContext& tick) const {
        SensorReading reading(runtime_->sensor.get());
        reading.source = ReadingSource::PROCESSING;
        reading.timestamp = tick.timestamp;

        return reading;
    }

public:
    SensorReaderBase(std::shared_ptr<ITimeService> time_service, const SensorRuntime& runtime) :
        time_service_(std::move(time_service)),
        runtime_(&runtime) {}

    ~SensorReaderBase() override = default;
};

} // namespace eerie_leap::domain::sensor_domain::sensor_readers
