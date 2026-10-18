#include <stdexcept>

#include "domain/sensor_domain/models/reading_status.h"
#include "domain/sensor_domain/models/sensor_reading.h"
#include "sensor_reader_physical_indicator.h"

namespace eerie_leap::domain::sensor_domain::sensor_readers {

using namespace eerie_leap::domain::sensor_domain::models;

SensorReaderPhysicalIndicator::SensorReaderPhysicalIndicator(
    std::shared_ptr<ITimeService> time_service,
    const SensorRuntime& runtime,
    std::shared_ptr<IGpio> gpio
) :
    SensorReaderBase(std::move(time_service), runtime),
    gpio_(std::move(gpio)) {

    if(GetSensor().configuration.type != SensorType::PHYSICAL_INDICATOR)
        throw std::runtime_error("Unsupported sensor type");
}

SensorReading SensorReaderPhysicalIndicator::Read(const TickContext& tick) {
    SensorReading reading = CreateReading(tick);

    const auto& channel = GetSensor().configuration.channel;
    if(!channel.has_value())
        throw std::runtime_error("Physical indicator sensor has no channel.");

    reading.value = gpio_->ReadChannel(static_cast<int>(*channel)) ? 1.0F : 0.0F;
    reading.raw_value = reading.value;
    reading.status = ReadingStatus::RAW;

    return reading;
}

} // namespace eerie_leap::domain::sensor_domain::sensor_readers
