#include <stdexcept>
#include <string>

#include "zephyr/kernel.h"
#include <zephyr/logging/log.h>

#include "gpio_sensor_reader.h"

LOG_MODULE_DECLARE(isr_sensor_reader_logger);

namespace eerie_leap::domain::sensor_domain::isr_sensor_readers {

using namespace eerie_leap::subsys::gpio;
using namespace eerie_leap::domain::sensor_domain::models;

GpioSensorReader::GpioSensorReader(
    std::shared_ptr<ITimeService> time_service,
    std::shared_ptr<GuidGenerator> guid_generator,
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame,
    std::shared_ptr<Sensor> sensor,
    ProcessSensorCallback process_sensor_callback,
    std::shared_ptr<WorkQueueThread> work_queue_thread,
    std::shared_ptr<IGpio> gpio)
        : IsrSensorReaderBase(
            std::move(time_service),
            std::move(guid_generator),
            std::move(sensor_readings_frame),
            std::move(sensor),
            std::move(process_sensor_callback)),
        work_queue_thread_(std::move(work_queue_thread)),
        gpio_(std::move(gpio)),
        dispatch_guard_(std::make_shared<IsrDispatchGuard<GpioSensorReader>>(this)) {

    if(sensor_->configuration.type != SensorType::PHYSICAL_INDICATOR)
        throw std::runtime_error("Unsupported sensor type");

    if(gpio_ == nullptr)
        throw std::runtime_error("GPIO is not available");

    if(!sensor_->configuration.channel.has_value())
        throw std::runtime_error("Sensor channel is not set");

    int channel = static_cast<int>(sensor_->configuration.channel.value());

    int handler_id = gpio_->RegisterChannelChangedHandler(
        channel,
        GpioEdge::BOTH,
        [this](int channel, bool state) {
            ARG_UNUSED(channel);

            QueueReading(state);
        });

    if(handler_id <= 0)
        throw std::runtime_error("Failed to register GPIO handler for channel: " + std::to_string(channel));

    channel_ = channel;
    handler_id_ = handler_id;

    // Seeds the frame, the channel level is only reported on edges afterwards.
    QueueReading(gpio_->ReadChannel(channel_));
}

GpioSensorReader::~GpioSensorReader() {
    if(handler_id_ > 0) {
        // Removal is serialised against dispatch, so no further edge is handed out.
        gpio_->RemoveChannelChangedHandler(channel_, handler_id_);

        handler_id_ = 0;
    }

    dispatch_guard_->Detach();
}

void GpioSensorReader::QueueReading(bool state) {
    if(!dispatch_guard_->TryAcquire())
        return;

    try {
        work_queue_thread_->Run(
            [guard = dispatch_guard_, state]() {
                guard->Dispatch([state](GpioSensorReader& reader) { reader.ProcessState(state); });

                // NOTE: Bouncing inputs flood processor
                // sleep is needed to let other threads to do work
                k_msleep(STATE_PROCESSING_DELAY_MS);

                guard->Release();
            });
    } catch(const std::exception& e) {
        // The queue rejects submissions while it is stopping, drop the edge.
        dispatch_guard_->Release();

        LOG_DBG("Gpio channel %d reading dropped: %s", channel_, e.what());
    }
}

// Exceptions must not unwind into the work queue's C dispatch.
void GpioSensorReader::ProcessState(bool state) noexcept {
    try {
        AddOrUpdateReading(state);
        process_sensor_callback_(*sensor_);
    } catch(const std::exception& e) {
        LOG_ERR("Gpio channel %d processing failed: %s", channel_, e.what());
    } catch(...) {
        LOG_ERR("Gpio channel %d processing failed.", channel_);
    }
}

void GpioSensorReader::AddOrUpdateReading(bool state) {
    SensorReading reading(guid_generator_->Generate(), sensor_);
    reading.source = ReadingSource::ISR;
    reading.timestamp = time_service_->GetCurrentTime();

    reading.value = static_cast<float>(state);
    reading.status = ReadingStatus::RAW;

    reading.metadata.AddTag<bool>(ReadingMetadataTag::RAW_VALUE, state);

    sensor_readings_frame_->AddOrUpdateReading(reading);
}

} // namespace eerie_leap::domain::sensor_domain::isr_sensor_readers
