#include <stdexcept>

#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/sensor_domain/models/reading_status.h"
#include "sensor_reader_virtual_indicator.h"

namespace eerie_leap::domain::sensor_domain::sensor_readers {

using namespace eerie_leap::domain::sensor_domain::models;

SensorReaderVirtualIndicator::SensorReaderVirtualIndicator(
    std::shared_ptr<ITimeService> time_service,
    std::shared_ptr<Sensor> sensor)
        : SensorReaderBase(std::move(time_service), std::move(sensor)) {

    if(sensor_->configuration.type != SensorType::VIRTUAL_INDICATOR)
        throw std::runtime_error("Unsupported sensor type");
}

// The value comes from the expression; the reader only stamps the reading.
SensorReading SensorReaderVirtualIndicator::Read() {
    SensorReading reading = CreateReading();
    reading.status = ReadingStatus::UNINITIALIZED;

    return reading;
}

} // namespace eerie_leap::domain::sensor_domain::sensor_readers
