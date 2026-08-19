#include "domain/canbus_domain/utilities/can_signal_codec.h"

#include "canbus_sensor_reader.h"

namespace eerie_leap::domain::sensor_domain::isr_sensor_readers {

using namespace eerie_leap::subsys::canbus;
using namespace eerie_leap::domain::sensor_domain::models;

using eerie_leap::domain::canbus_domain::utilities::CanSignalCodec;

CanbusSensorReader::CanbusSensorReader(
    std::shared_ptr<ITimeService> time_service,
    std::shared_ptr<GuidGenerator> guid_generator,
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
    std::shared_ptr<Sensor> sensor,
    ProcessSensorCallback process_sensor_callback,
    std::shared_ptr<WorkQueueThread> work_queue_thread,
    std::shared_ptr<CanbusProxy> canbus,
    std::shared_ptr<const CanSignalConfiguration> signal_configuration)
        : CanbusSensorReaderRaw(
            std::move(time_service),
            std::move(guid_generator),
            std::move(sensor_readings_frame),
            std::move(sensor),
            std::move(process_sensor_callback),
            std::move(work_queue_thread),
            std::move(canbus)
        ),
        signal_configuration_(std::move(signal_configuration)) {}

CanbusSensorReader::~CanbusSensorReader() {
    Detach();
}

void CanbusSensorReader::AddOrUpdateReading(const CanFrame& can_frame) {
    auto reading = CreateRawReading(can_frame);
    if(!reading)
        return;

    auto value = CanSignalCodec::Decode(*signal_configuration_, can_frame.data);
    if(!value)
        return;

    reading.value().value = value.value();

    if(reading.value().sensor->configuration.type == SensorType::CANBUS_ANALOG)
        reading.value().metadata.AddTag<float>(ReadingMetadataTag::RAW_VALUE, reading.value().value.value());
    else if(reading.value().sensor->configuration.type == SensorType::CANBUS_INDICATOR)
        reading.value().metadata.AddTag<bool>(ReadingMetadataTag::RAW_VALUE, reading.value().value.value() > 0);

    sensor_readings_frame_->AddOrUpdateReading(reading.value());
}

} // namespace eerie_leap::domain::sensor_domain::isr_sensor_readers
