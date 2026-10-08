#include <stdexcept>
#include <string>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "gpio_sensor_reader.h"

LOG_MODULE_REGISTER(isr_sensor_reader_logger);

namespace eerie_leap::domain::sensor_domain::isr_sensor_readers {

using namespace eerie_leap::subsys::gpio;
using namespace eerie_leap::domain::sensor_domain::models;

GpioSensorReader::GpioSensorReader(
    std::shared_ptr<ITimeService> time_service,
    const SensorRuntime& runtime,
    ProcessSensorCallback process_sensor_callback,
    std::shared_ptr<WorkQueueThread> work_queue_thread,
    std::shared_ptr<IGpio> gpio)
        : IsrSensorReaderBase(
            std::move(time_service),
            runtime,
            std::move(process_sensor_callback)),
        work_queue_thread_(std::move(work_queue_thread)),
        gpio_(std::move(gpio)) {

    if(GetSensor().configuration.type != SensorType::PHYSICAL_INDICATOR)
        throw std::runtime_error("Unsupported sensor type");

    if(gpio_ == nullptr)
        throw std::runtime_error("GPIO is not available");

    if(!GetSensor().configuration.channel.has_value())
        throw std::runtime_error("Sensor channel is not set");

    int channel = static_cast<int>(GetSensor().configuration.channel.value());

    pending_work_.reader = this;
    k_work_init(&pending_work_.work, WorkHandler);

    // Handler removal is serialised against dispatch, so `this` outlives every call.
    int handler_id = gpio_->RegisterChannelChangedHandler(
        channel,
        GpioEdge::BOTH,
        [this](int channel, bool state) {
            ARG_UNUSED(channel);

            QueueState(state);
        });

    if(handler_id <= 0)
        throw std::runtime_error("Failed to register GPIO handler for channel: " + std::to_string(channel));

    channel_ = channel;
    handler_id_ = handler_id;

    // Seeds the frame, the channel level is only reported on edges afterwards.
    QueueState(gpio_->ReadChannel(channel_));
}

GpioSensorReader::~GpioSensorReader() {
    if(handler_id_ > 0) {
        gpio_->RemoveChannelChangedHandler(channel_, handler_id_);

        handler_id_ = 0;
    }

    // Waits for a running dispatch; nothing submits afterwards since the handler is gone.
    k_work_cancel_sync(&pending_work_.work, &work_sync_);
}

// Caller's thread. Keeps only the newest level while the work queue is behind.
void GpioSensorReader::QueueState(bool state) {
    atomic_set(&pending_state_, state ? 1 : 0);

    // Returns 0 while already queued; a stopping queue rejects the submission and the edge is dropped.
    try {
        if(k_work_submit_to_queue(work_queue_thread_->GetWorkQueue(), &pending_work_.work) < 0)
            LOG_DBG("Gpio channel %d reading dropped: work queue unavailable.", channel_);
    } catch(const std::exception& e) {
        LOG_DBG("Gpio channel %d reading dropped: %s", channel_, e.what());
    }
}

void GpioSensorReader::WorkHandler(k_work* work) {
    auto* pending_work = CONTAINER_OF(work, PendingWork, work);
    pending_work->reader->ProcessPendingState();
}

// Exceptions must not unwind into the work queue's C dispatch.
void GpioSensorReader::ProcessPendingState() noexcept {
    const bool state = atomic_get(&pending_state_) != 0;

    try {
        SensorReading reading = CreateReading();
        reading.value = state ? 1.0F : 0.0F;
        reading.raw_value = reading.value;
        reading.status = ReadingStatus::RAW;

        process_sensor_callback_(*runtime_, reading);
    } catch(const std::exception& e) {
        LOG_ERR("Gpio channel %d processing failed: %s", channel_, e.what());
    } catch(...) {
        LOG_ERR("Gpio channel %d processing failed.", channel_);
    }
}

} // namespace eerie_leap::domain::sensor_domain::isr_sensor_readers
