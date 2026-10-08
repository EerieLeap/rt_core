#include "domain/canbus_domain/utilities/can_signal_codec.h"

#include "canbus_sensor_reader.h"

namespace eerie_leap::domain::sensor_domain::isr_sensor_readers {

using namespace eerie_leap::subsys::canbus;
using namespace eerie_leap::domain::sensor_domain::models;

using eerie_leap::domain::canbus_domain::utilities::CanSignalCodec;

CanbusSensorReader::CanbusSensorReader(
    std::shared_ptr<ITimeService> time_service,
    const SensorRuntime& runtime,
    ProcessSensorCallback process_sensor_callback,
    std::shared_ptr<WorkQueueThread> work_queue_thread,
    std::shared_ptr<CanbusProxy> canbus,
    const CanId& frame_id,
    std::shared_ptr<const CanSignalConfiguration> signal_configuration)
        : CanbusSensorReaderRaw(
            std::move(time_service),
            runtime,
            std::move(process_sensor_callback),
            std::move(work_queue_thread),
            std::move(canbus),
            frame_id
        ),
        signal_configuration_(std::move(signal_configuration)) {}

CanbusSensorReader::~CanbusSensorReader() {
    Detach();
}

void CanbusSensorReader::FillReading(SensorReading& reading, const CanFrame& can_frame) {
    reading.status = ReadingStatus::RAW;

    auto value = CanSignalCodec::Decode(*signal_configuration_, can_frame.data);
    if(!value.has_value()) {
        reading.SetError(ReadingError::READER_FAILED);
        return;
    }

    reading.value = value.value();
    reading.raw_value = value.value();
}

} // namespace eerie_leap::domain::sensor_domain::isr_sensor_readers
