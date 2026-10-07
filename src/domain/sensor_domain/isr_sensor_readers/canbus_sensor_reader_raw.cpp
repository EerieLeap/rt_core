#include <stdexcept>
#include <string>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "canbus_sensor_reader_raw.h"

LOG_MODULE_REGISTER(isr_sensor_reader_logger);

namespace eerie_leap::domain::sensor_domain::isr_sensor_readers {

using namespace eerie_leap::subsys::canbus;
using namespace eerie_leap::domain::sensor_domain::models;

CanbusSensorReaderRaw::CanbusSensorReaderRaw(
    std::shared_ptr<ITimeService> time_service,
    std::shared_ptr<Sensor> sensor,
    ProcessSensorCallback process_sensor_callback,
    std::shared_ptr<WorkQueueThread> work_queue_thread,
    std::shared_ptr<CanbusProxy> canbus,
    const CanId& frame_id)
        : IsrSensorReaderBase(
            std::move(time_service),
            std::move(sensor),
            std::move(process_sensor_callback)),
        work_queue_thread_(std::move(work_queue_thread)),
        canbus_(std::move(canbus)) {

    if(!canbus_->IsValid())
        throw std::runtime_error("CANBus proxy is not valid");

    pending_work_.reader = this;
    k_work_init(&pending_work_.work, WorkHandler);

    // Handler removal is serialised against dispatch, so `this` outlives every call.
    int handler_id = (*canbus_)->RegisterFrameReceivedHandler(
        frame_id,
        [this](const CanFrame& frame) { OnFrameReceived(frame); });

    if(handler_id <= 0)
        throw std::runtime_error("Failed to register CAN frame handler for frame ID: " + std::to_string(frame_id.id));

    frame_handler_id_ = handler_id;
}

CanbusSensorReaderRaw::~CanbusSensorReaderRaw() {
    Detach();
}

void CanbusSensorReaderRaw::Detach() {
    if(frame_handler_id_ > 0) {
        if(auto* canbus = canbus_->Get(); canbus != nullptr)
            canbus->RemoveFrameReceivedHandler(frame_handler_id_);

        frame_handler_id_ = 0;
    }

    // Waits for a running dispatch; nothing submits afterwards since the handler is gone.
    k_work_cancel_sync(&pending_work_.work, &work_sync_);
}

// CAN thread. Keeps only the newest frame while the work queue is behind.
void CanbusSensorReaderRaw::OnFrameReceived(const CanFrame& can_frame) {
    if(can_frame.data.empty())
        return;

    K_SPINLOCK(&pending_lock_) {
        pending_frame_ = can_frame;
        has_pending_frame_ = true;
    }

    // Returns 0 while already queued; a stopping queue rejects the submission and the frame is dropped.
    try {
        if(k_work_submit_to_queue(work_queue_thread_->GetWorkQueue(), &pending_work_.work) < 0)
            LOG_DBG("CAN frame ID 0x%08X dropped: work queue unavailable.", can_frame.id);
    } catch(const std::exception& e) {
        LOG_DBG("CAN frame ID 0x%08X dropped: %s", can_frame.id, e.what());
    }
}

void CanbusSensorReaderRaw::WorkHandler(k_work* work) {
    auto* pending_work = CONTAINER_OF(work, PendingWork, work);
    pending_work->reader->ProcessPendingFrame();
}

// Exceptions must not unwind into the work queue's C dispatch.
void CanbusSensorReaderRaw::ProcessPendingFrame() noexcept {
    CanFrame can_frame;
    bool has_frame = false;

    K_SPINLOCK(&pending_lock_) {
        has_frame = has_pending_frame_;
        if(has_frame) {
            can_frame = pending_frame_;
            has_pending_frame_ = false;
        }
    }

    if(!has_frame)
        return;

    try {
        SensorReading reading = CreateReading();
        FillReading(reading, can_frame);
        process_sensor_callback_(*sensor_, reading);
    } catch(const std::exception& e) {
        LOG_ERR("CAN frame ID 0x%08X processing failed: %s", can_frame.id, e.what());
    } catch(...) {
        LOG_ERR("CAN frame ID 0x%08X processing failed.", can_frame.id);
    }
}

void CanbusSensorReaderRaw::FillReading(SensorReading& reading, const CanFrame& can_frame) {
    reading.status = ReadingStatus::RAW;
    reading.can_frame = &can_frame;
}

} // namespace eerie_leap::domain::sensor_domain::isr_sensor_readers
