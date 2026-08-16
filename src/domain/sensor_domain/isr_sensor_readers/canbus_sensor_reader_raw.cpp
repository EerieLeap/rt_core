#include <stdexcept>

#include "zephyr/kernel.h"

#include "canbus_sensor_reader_raw.h"

namespace eerie_leap::domain::sensor_domain::isr_sensor_readers {

using namespace eerie_leap::subsys::canbus;
using namespace eerie_leap::domain::sensor_domain::models;

CanbusSensorReaderRaw::CanbusSensorReaderRaw(
    std::shared_ptr<ITimeService> time_service,
    std::shared_ptr<GuidGenerator> guid_generator,
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
    std::shared_ptr<Sensor> sensor,
    ProcessSensorCallback process_sensor_callback,
    std::shared_ptr<WorkQueueThread> work_queue_thread,
    std::shared_ptr<CanbusProxy> canbus)
        : IsrSensorReaderBase(
            std::move(time_service),
            std::move(guid_generator),
            std::move(sensor_readings_frame),
            std::move(sensor),
            std::move(process_sensor_callback)),
        work_queue_thread_(std::move(work_queue_thread)),
        canbus_(std::move(canbus)) {

    k_sem_init(&processing_semaphore_, 1, 1);

    uint32_t frame_id = sensor_->configuration.canbus_source->frame_id;

    if(!canbus_->IsValid())
        throw std::runtime_error("CANBus proxy is not valid");

    int handler_id = (*canbus_)->RegisterFrameReceivedHandler(
        frame_id,
        [this](const CanFrame& frame) {
            if(k_sem_take(&processing_semaphore_, K_NO_WAIT) != 0)
                return;

            work_queue_thread_->Run(
                [this, frame]() {
                    if(atomic_get(&is_destroying_)) {
                        k_sem_give(&processing_semaphore_);
                        return;
                    }

                    AddOrUpdateReading(frame);
                    process_sensor_callback_(*sensor_);

                    // NOTE: High incoming frame rate floods processor
                    // sleep is needed to let other threads to do work
                    k_msleep(FRAME_PROCESSING_DELAY_MS);

                    k_sem_give(&processing_semaphore_);
                });
        });

    if(handler_id <= 0)
        throw std::runtime_error("Failed to register CAN frame handler for frame ID: " + std::to_string(frame_id));

    frame_id_ = frame_id;
    frame_handler_id_ = handler_id;
}

CanbusSensorReaderRaw::~CanbusSensorReaderRaw() {
    atomic_set(&is_destroying_, 1);

    if(auto* canbus = canbus_->Get(); canbus != nullptr)
        canbus->RemoveFrameReceivedHandler(frame_id_, frame_handler_id_);

    k_sem_take(&processing_semaphore_, K_MSEC(100));
    k_sem_give(&processing_semaphore_);
}

std::optional<SensorReading> CanbusSensorReaderRaw::CreateRawReading(const CanFrame& can_frame) {
    if(can_frame.data.empty())
        return std::nullopt;

    SensorReading reading(guid_generator_->Generate(), sensor_);
    reading.source = ReadingSource::ISR;
    reading.timestamp = time_service_->GetCurrentTime();

    reading.value = std::nullopt;
    reading.status = ReadingStatus::RAW;

    reading.metadata.AddTag<CanFrame>(
        ReadingMetadataTag::CANBUS_DATA,
        can_frame);

    return reading;
}

void CanbusSensorReaderRaw::AddOrUpdateReading(const CanFrame& can_frame) {
    auto reading = CreateRawReading(can_frame);
    if(!reading)
        return;

    sensor_readings_frame_->AddOrUpdateReading(reading.value());
}

} // namespace eerie_leap::domain::sensor_domain::isr_sensor_readers
