#include <stdexcept>

#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/sensor_domain/models/reading_status.h"
#include "sensor_reader_physical_indicator.h"

namespace eerie_leap::domain::sensor_domain::sensor_readers {

using namespace eerie_leap::domain::sensor_domain::models;

SensorReaderPhysicalIndicator::SensorReaderPhysicalIndicator(
    std::shared_ptr<ITimeService> time_service,
    std::shared_ptr<Sensor> sensor,
    std::shared_ptr<IGpio> gpio)
        : SensorReaderBase(std::move(time_service), std::move(sensor)),
        gpio_(std::move(gpio)) {

    if(sensor_->configuration.type != SensorType::PHYSICAL_INDICATOR)
        throw std::runtime_error("Unsupported sensor type");
}

SensorReading SensorReaderPhysicalIndicator::Read() {
    SensorReading reading = CreateReading();

    reading.value = gpio_->ReadChannel(sensor_->configuration.channel.value()) ? 1.0F : 0.0F;
    reading.raw_value = reading.value;
    reading.status = ReadingStatus::RAW;

    return reading;
}

} // namespace eerie_leap::domain::sensor_domain::sensor_readers
