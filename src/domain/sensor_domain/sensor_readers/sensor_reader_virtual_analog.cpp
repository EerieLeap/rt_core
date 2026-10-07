#include <stdexcept>

#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/sensor_domain/models/reading_status.h"
#include "sensor_reader_virtual_analog.h"

namespace eerie_leap::domain::sensor_domain::sensor_readers {

using namespace eerie_leap::domain::sensor_domain::models;

SensorReaderVirtualAnalog::SensorReaderVirtualAnalog(
    std::shared_ptr<ITimeService> time_service,
    std::shared_ptr<Sensor> sensor)
        : SensorReaderBase(std::move(time_service), std::move(sensor)) {

    if(sensor_->configuration.type != SensorType::VIRTUAL_ANALOG)
        throw std::runtime_error("Unsupported sensor type");
}

// The value comes from the expression; the reader only stamps the reading.
SensorReading SensorReaderVirtualAnalog::Read() {
    SensorReading reading = CreateReading();
    reading.status = ReadingStatus::UNINITIALIZED;

    return reading;
}

} // namespace eerie_leap::domain::sensor_domain::sensor_readers
