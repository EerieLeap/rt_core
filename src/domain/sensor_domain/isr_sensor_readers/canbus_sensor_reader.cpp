#include "canbus_sensor_reader.h"

namespace eerie_leap::domain::sensor_domain::isr_sensor_readers {

using namespace eerie_leap::subsys::canbus;

CanbusSensorReader::CanbusSensorReader(
    std::shared_ptr<ITimeService> time_service,
    std::shared_ptr<GuidGenerator> guid_generator,
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
    std::shared_ptr<Sensor> sensor,
    ProcessSensorCallback process_sensor_callback,
    std::shared_ptr<WorkQueueThread> work_queue_thread,
    std::shared_ptr<CanbusProxy> canbus,
    std::shared_ptr<Dbc> dbc)
        : CanbusSensorReaderRaw(
            std::move(time_service),
            std::move(guid_generator),
            std::move(sensor_readings_frame),
            std::move(sensor),
            std::move(process_sensor_callback),
            std::move(work_queue_thread),
            std::move(canbus)
        ),
        dbc_(std::move(dbc)) {}

void CanbusSensorReader::AddOrUpdateReading(const CanFrame can_frame) {
    auto reading = CreateRawReading(can_frame);
    if(!reading)
        return;

    reading.value().value = dbc_->GetMessage(sensor_->configuration.canbus_source->frame_id)->GetSignalValue(
        sensor_->configuration.canbus_source->signal_name_hash,
        can_frame.data.data());

    if(reading.value().sensor->configuration.type == SensorType::CANBUS_ANALOG)
        reading.value().metadata.AddTag<float>(ReadingMetadataTag::RAW_VALUE, reading.value().value.value());
    else if(reading.value().sensor->configuration.type == SensorType::CANBUS_INDICATOR)
        reading.value().metadata.AddTag<bool>(ReadingMetadataTag::RAW_VALUE, reading.value().value.value() > 0);

    sensor_readings_frame_->AddOrUpdateReading(reading.value());
}

} // namespace eerie_leap::domain::sensor_domain::isr_sensor_readers
