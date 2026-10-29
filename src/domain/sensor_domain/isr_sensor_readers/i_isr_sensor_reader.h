#pragma once

#include <functional>

#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/sensor_domain/runtime/sensor_runtime.h"

namespace eerie_leap::domain::sensor_domain::isr_sensor_readers {

using eerie_leap::domain::sensor_domain::models::SensorReading;
using eerie_leap::domain::sensor_domain::runtime::SensorRuntime;

// Receives a reading the reader produced, on the reader's work queue, and commits it.
using ProcessSensorCallback = std::function<void(const SensorRuntime&, SensorReading&)>;

class IIsrSensorReader {
public:
    virtual ~IIsrSensorReader() = default;

    // Processing pauses. Never from the processing work queue. A reader that returns true keeps
    // listening and processes nothing until Resume(); one that returns false is torn down and built
    // again when processing resumes, which is how a source reads its level again then.
    virtual bool Pause() {
        return false;
    }

    // Processing resumes, for a reader whose Pause() returned true.
    virtual void Resume() {}
};

} // namespace eerie_leap::domain::sensor_domain::isr_sensor_readers
