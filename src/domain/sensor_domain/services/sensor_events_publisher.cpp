#include <exception>
#include <utility>

#include <zephyr/logging/log.h>

#include "utilities/reflection/caller_name.h"

#include "sensor_events_publisher.h"

namespace eerie_leap::domain::sensor_domain::services {

using eerie_leap::domain::sensor_domain::event_bus::SensorEventType;
using eerie_leap::domain::sensor_domain::event_bus::SensorPayloadType;
using eerie_leap::utilities::reflection::GetCallerName;

LOG_MODULE_REGISTER(sensor_events_publisher_logger);

SensorEventsPublisher::SensorEventsPublisher(std::shared_ptr<SensorReadingsFrame> sensor_readings_frame) :
    sensor_readings_frame_(std::move(sensor_readings_frame)) {}

// The task refers to this publisher, so it is stopped before anything goes.
SensorEventsPublisher::~SensorEventsPublisher() {
    Stop();
}

bool SensorEventsPublisher::DoInitialize() {
    if(work_queue_thread_ != nullptr)
        return true;

    if(sensor_readings_frame_ == nullptr) {
        LOG_ERR("The sensor events publisher needs a readings frame.");
        return false;
    }

    auto work_queue_thread = std::make_unique<WorkQueueThread>(
        "sensor_events_publisher", CONFIG_EERIE_LEAP_DOMAIN_SENSOR_EVENTS_PUBLISHER_STACK_SIZE,
        CONFIG_EERIE_LEAP_DOMAIN_SENSOR_EVENTS_PUBLISHER_PRIORITY
    );
    if(!work_queue_thread->Initialize()) {
        LOG_ERR("Failed to initialize the sensor events publisher work queue.");
        return false;
    }

    work_queue_thread_ = std::move(work_queue_thread);
    work_queue_task_ = work_queue_thread_->CreateTask(ProcessWorkTask, this);

    return true;
}

bool SensorEventsPublisher::DoStart() {
    if(!work_queue_task_.has_value()) {
        LOG_ERR("The sensor events publisher was started before it was initialized.");
        return false;
    }

    work_queue_task_->Schedule();

    LOG_INF("Sensor events publisher started.");

    return true;
}

bool SensorEventsPublisher::DoStop() {
    CancelTask();

    LOG_INF("Sensor events publisher stopped.");

    return true;
}

bool SensorEventsPublisher::DoPause() {
    CancelTask();

    return true;
}

bool SensorEventsPublisher::DoResume() {
    if(!work_queue_task_.has_value())
        return false;

    work_queue_task_->Schedule();

    return true;
}

void SensorEventsPublisher::CancelTask() {
    if(!work_queue_task_.has_value())
        return;

    // Cancel() waits for a tick in progress; the loop takes the reschedule that tick made.
    while(work_queue_task_->Cancel())
        k_sleep(K_MSEC(1));
}

void SensorEventsPublisher::Replay() {
    if(sensor_readings_frame_ != nullptr)
        sensor_readings_frame_->MarkProcessedReadingsUpdated();
}

WorkQueueTaskResult SensorEventsPublisher::ProcessWorkTask(SensorEventsPublisher* publisher) {
    // Caught here so the task stays periodic even when its first tick throws. Nothing in a tick is
    // expected to, so a throw is a bug; logged once per run of failing ticks, not every period.
    try {
        publisher->PublishProcessedReadings();
        publisher->is_tick_error_logged_ = false;
    } catch(const std::exception& e) {
        if(!std::exchange(publisher->is_tick_error_logged_, true))
            LOG_ERR("Failed to publish sensor events. %s", e.what());
    } catch(...) {
        if(!std::exchange(publisher->is_tick_error_logged_, true))
            LOG_ERR("Failed to publish sensor events: a non-standard exception.");
    }

    return { .reschedule = true, .delay = K_MSEC(CONFIG_EERIE_LEAP_DOMAIN_SENSOR_EVENTS_PUBLISHER_PERIOD_MS) };
}

void SensorEventsPublisher::PublishProcessedReadings() {
    static constexpr auto CALLER = GetCallerName();

    auto& channel = SensorEventsChannel::GetInstance();

    // Taken in one step: a separate clear would drop readings processed in between. Readings that
    // do not fit the buffer stay pending for the next take, which comes in this tick if the channel
    // is empty by then.
    size_t count = 0;
    do {
        // Before each take, never after one, which clears the readings' marks. While the channel holds
        // events (a subscriber is stalled, say a widget waiting for the LVGL lock) the readings stay
        // marked in the frame, and the first tick to find the channel empty publishes each sensor's
        // newest value, a pending Replay() included. Queued behind an event, readings would push it
        // out once the queue is full.
        if(channel.GetQueuedCount() != 0)
            return;

        count = sensor_readings_frame_->TakeProcessedReadings(readings_);

        // Read after each take, never before: whoever subscribed and then committed or replayed
        // (Replay()) has bumped the generation before releasing the frame lock this take acquired,
        // so the readings just taken are never judged by an answer given before that subscriber.
        const uint32_t subscription_generation = channel.GetSubscriptionGeneration();
        acceptance_cache_.Revalidate(subscription_generation);

        for(size_t i = 0; i < count; i++) {
            const auto& reading = readings_[i];

            // A reading without a value (a raw CAN frame) has nothing to publish.
            if(!reading.value.has_value())
                continue;

            const SensorEventsChannel::EventMessage event{ .source_id = CALLER.hash,
                .type = SensorEventType::DataUpdated,
                .payload = { { SensorPayloadType::SensorId, reading.sensor_id_hash },
                    { SensorPayloadType::Value, *reading.value } } };

            if(IsAccepted(channel, event, reading.sensor_id_hash, subscription_generation))
                channel.PublishAsync(event);
        }
    } while(count == readings_.size());
}

bool SensorEventsPublisher::IsAccepted(
    const SensorEventsChannel& channel,
    const SensorEventsChannel::EventMessage& event,
    uint32_t sensor_id_hash,
    uint32_t subscription_generation
) {

    if(const auto is_accepted = acceptance_cache_.Find(sensor_id_hash); is_accepted.has_value())
        return *is_accepted;

    // A filter that throws counts as a rejection and is cached as one, so it is asked again only
    // after the next subscription change, and the rest of the readings already taken still go out.
    bool is_accepted = false;
    try {
        is_accepted = channel.Accepts(event);
    } catch(const std::exception& e) {
        LogFilterError(sensor_id_hash, subscription_generation, e.what());
    } catch(...) {
        LogFilterError(sensor_id_hash, subscription_generation, "a non-standard exception");
    }

    acceptance_cache_.Store(sensor_id_hash, is_accepted);

    return is_accepted;
}

void SensorEventsPublisher::LogFilterError(
    uint32_t sensor_id_hash,
    uint32_t subscription_generation,
    const char* error
) {
    if(filter_error_generation_ == subscription_generation)
        return;

    filter_error_generation_ = subscription_generation;

    LOG_ERR(
        "A sensor_events filter threw for sensor 0x%08x, which is not published until the subscriptions change. %s",
        sensor_id_hash, error
    );
}

} // namespace eerie_leap::domain::sensor_domain::services
