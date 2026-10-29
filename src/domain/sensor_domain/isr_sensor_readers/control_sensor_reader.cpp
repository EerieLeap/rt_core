#include <cmath>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "domain/sensor_domain/event_bus/sensor_event_type.h"
#include "domain/sensor_domain/event_bus/sensor_payload_type.h"
#include "domain/sensor_domain/models/sensor_type_traits.h"
#include "subsys/event_bus/event_binding.h"
#include "subsys/event_bus/event_binding_subscription.h"

#include "control_sensor_reader.h"

LOG_MODULE_REGISTER(control_sensor_reader_logger);

namespace eerie_leap::domain::sensor_domain::isr_sensor_readers {

using namespace eerie_leap::domain::sensor_domain::models;

using eerie_leap::domain::sensor_domain::event_bus::SensorEventType;
using eerie_leap::domain::sensor_domain::event_bus::SensorPayloadType;
using eerie_leap::subsys::event_bus::EventBinding;
using eerie_leap::subsys::event_bus::EventChannelId;
using eerie_leap::subsys::event_bus::SubscribeEventBinding;

namespace {

// A bool as 1 or 0 and a number as it is. Text and NaN are no input.
std::optional<float> ReadInput(const EventData& value) {
    if(const auto* flag = std::get_if<bool>(&value))
        return *flag ? 1.0F : 0.0F;

    if(const auto* number = std::get_if<int>(&value))
        return static_cast<float>(*number);

    if(const auto* unsigned_number = std::get_if<uint32_t>(&value))
        return static_cast<float>(*unsigned_number);

    if(const auto* real = std::get_if<float>(&value))
        return std::isnan(*real) ? std::nullopt : std::optional<float>(*real);

    return std::nullopt;
}

} // namespace

ControlSensorReader::ControlSensorReader(
    std::shared_ptr<ITimeService> time_service,
    const SensorRuntime& runtime,
    ProcessSensorCallback process_sensor_callback,
    std::shared_ptr<WorkQueueThread> work_queue_thread
) :
    IsrSensorReaderBase(std::move(time_service), runtime, std::move(process_sensor_callback)),
    work_queue_thread_(std::move(work_queue_thread)),
    guard_(std::make_shared<DispatchGuard>(this)) {

    const auto traits = GetSensor().configuration.GetTraits();
    if(traits.source != SensorSourceKind::CONTROL)
        throw std::runtime_error("Unsupported sensor type");

    is_indicator_ = traits.value_kind == SensorValueKind::INDICATOR;

    pending_work_.reader = this;
    k_work_init(&pending_work_.work, WorkHandler);

    const EventBinding binding{ .channel = EventChannelId::Sensors,
        .event_type = std::to_underlying(SensorEventType::InputRequested),
        .payload_key = std::to_underlying(SensorPayloadType::Value),
        .selector_key = std::to_underlying(SensorPayloadType::SensorId),
        .selector_value = std::pmr::string(GetSensor().id) };

    // The handler holds the guard, never the reader: a delivery the channel took before the
    // subscription went becomes a no-op once the guard is detached.
    subscription_ = SubscribeEventBinding(binding, [guard = guard_, this](const EventData& value) {
        guard->Dispatch([this, &value] {
            OnInput(value);
        });
    });

    if(subscription_ == nullptr)
        throw std::runtime_error("Failed to subscribe to the input of sensor: " + std::string(GetSensor().id));
}

ControlSensorReader::~ControlSensorReader() {
    // Waits for a handler in its body; after that none submits the work item.
    guard_->Detach();
    subscription_.reset();

    // Waits for a running work item and drops a queued one with its values.
    k_work_cancel_sync(&pending_work_.work, &work_sync_);
}

// Bus thread, or the publisher's for a synchronous Publish().
void ControlSensorReader::OnInput(const EventData& value) {
    const auto input = ReadInput(value);
    if(!input.has_value()) {
        // Once: a control that publishes such values would flood the log.
        if(!is_unreadable_logged_) {
            is_unreadable_logged_ = true;
            LOG_WRN("Control sensor %s ignores a value that is not a number.", GetSensor().id.c_str());
        }

        return;
    }

    // An indicator takes the number as a level: anything other than 0 is on.
    float number = *input;
    if(is_indicator_)
        number = number != 0.0F ? 1.0F : 0.0F;

    QueueInput(number);
}

void ControlSensorReader::QueueInput(float value) {
    bool is_dropped = false;
    bool is_paused = false;

    K_SPINLOCK(&lock_) {
        is_paused = is_paused_;

        if(count_ > 0 && (is_paused || !is_indicator_)) {
            // The newest value replaces the one still waiting: an analog sensor's always, and an
            // indicator's during a pause, when only the level at the resume counts.
            queue_[(head_ + count_ - 1) % queue_.size()] = value;
        } else if(count_ == queue_.size()) {
            is_dropped = true;
        } else {
            queue_[(head_ + count_) % queue_.size()] = value;
            count_++;
        }
    }

    if(is_dropped) {
        atomic_inc(&dropped_count_);
        LOG_WRN(
            "Control sensor %s drops an input: %zu are waiting for the processing queue.", GetSensor().id.c_str(),
            queue_.size()
        );

        return;
    }

    // Held until Resume().
    if(is_paused)
        return;

    SubmitWork();
}

// Returns 0 while already queued; a stopping queue rejects the submission, and the value waits.
void ControlSensorReader::SubmitWork() {
    try {
        if(k_work_submit_to_queue(work_queue_thread_->GetWorkQueue(), &pending_work_.work) < 0)
            LOG_DBG("Control sensor %s input not processed: work queue unavailable.", GetSensor().id.c_str());
    } catch(const std::exception& e) {
        LOG_DBG("Control sensor %s input not processed: %s", GetSensor().id.c_str(), e.what());
    }
}

bool ControlSensorReader::Pause() {
    K_SPINLOCK(&lock_) {
        is_paused_ = true;

        // The newest value goes to the front.
        if(count_ > 1) {
            queue_[head_] = queue_[(head_ + count_ - 1) % queue_.size()];
            count_ = 1;
        }
    }

    // A work item that was submitted before the pause finds it marked and processes nothing.
    k_work_cancel_sync(&pending_work_.work, &work_sync_);

    return true;
}

void ControlSensorReader::Resume() {
    bool has_value = false;

    K_SPINLOCK(&lock_) {
        is_paused_ = false;
        has_value = count_ > 0;
    }

    if(has_value)
        SubmitWork();
}

void ControlSensorReader::WorkHandler(k_work* work) {
    const auto* pending_work = CONTAINER_OF(work, PendingWork, work);
    pending_work->reader->ProcessPendingInputs();
}

// Exceptions must not unwind into the work queue's C dispatch.
void ControlSensorReader::ProcessPendingInputs() noexcept {
    while(true) {
        float value = 0.0F;
        bool has_value = false;

        K_SPINLOCK(&lock_) {
            has_value = count_ > 0 && !is_paused_;
            if(has_value) {
                value = queue_[head_];
                head_ = (head_ + 1) % queue_.size();
                count_--;
            }
        }

        if(!has_value)
            return;

        try {
            SensorReading reading = CreateReading();
            reading.value = value;
            reading.raw_value = value;
            reading.status = ReadingStatus::RAW;

            process_sensor_callback_(*runtime_, reading);
        } catch(const std::exception& e) {
            LOG_ERR("Control sensor %s processing failed: %s", GetSensor().id.c_str(), e.what());
        } catch(...) {
            LOG_ERR("Control sensor %s processing failed.", GetSensor().id.c_str());
        }
    }
}

} // namespace eerie_leap::domain::sensor_domain::isr_sensor_readers
