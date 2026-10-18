#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "domain/sensor_domain/event_bus/sensor_event_bus.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/sensor_domain/models/sensor_type.h"
#include "domain/sensor_domain/services/sensor_events_publisher.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "subsys/event_bus/event_binding.h"
#include "subsys/event_bus/event_binding_subscription.h"
#include "subsys/event_bus/event_channel_registry.h"
#include "subsys/threading/scoped_mutex.h"
#include "utilities/memory/memory_resource_manager.h"

// Shared by the publisher suite and the allocation-free case, which must live in the one unit that
// includes the counting allocator. Round() and the Recorder never allocate, so a round can run
// inside an allocation window; bind and build sensors before the window opens.
//
// A round is exactly one publisher tick: Pause() cancels the next tick synchronously, the commits
// land while nothing takes, and Resume() schedules a tick at once that drains them all. The
// sentinel sits in the frame's last slot and every round commits it, so its delivery comes after
// every other event of the round: the tick publishes in slot order and the single bus thread
// dispatches each event to all its subscribers before the next.
namespace sensor_events_test_support {

using eerie_leap::domain::sensor_domain::event_bus::SensorEventBus;
using eerie_leap::domain::sensor_domain::event_bus::SensorEventsChannel;
using eerie_leap::domain::sensor_domain::event_bus::SensorEventType;
using eerie_leap::domain::sensor_domain::event_bus::SensorPayloadType;
using eerie_leap::domain::sensor_domain::models::ReadingSource;
using eerie_leap::domain::sensor_domain::models::ReadingStatus;
using eerie_leap::domain::sensor_domain::models::Sensor;
using eerie_leap::domain::sensor_domain::models::SensorReading;
using eerie_leap::domain::sensor_domain::models::SensorType;
using eerie_leap::domain::sensor_domain::services::SensorEventsPublisher;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;
using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::event_bus::EventBinding;
using eerie_leap::subsys::event_bus::EventChannelId;
using eerie_leap::subsys::event_bus::EventChannelRegistry;
using eerie_leap::subsys::event_bus::EventData;
using eerie_leap::subsys::event_bus::SubscribeEventBinding;
using eerie_leap::subsys::threading::ScopedMutex;
using eerie_leap::utilities::memory::Mrm;

// A failure bound only; nothing waits on time to synchronise.
inline constexpr int WAIT_MS = 1000;

// The values one subscriber received, in order. Written on the bus thread and read by the test,
// and held by its handler through a shared_ptr, so a late delivery never reaches a dead frame.
class Recorder {
public:
    static constexpr size_t CAPACITY = 32;

private:
    mutable k_mutex mutex_{};
    k_sem delivered_{};
    std::array<float, CAPACITY> values_{};
    size_t count_ = 0; // Every delivery, also past the capacity.

public:
    Recorder() {
        k_mutex_init(&mutex_);
        k_sem_init(&delivered_, 0, K_SEM_MAX_LIMIT);
    }

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;
    ~Recorder() = default;

    void Record(float value) {
        {
            ScopedMutex guard(mutex_);

            if(count_ < CAPACITY)
                values_[count_] = value;

            count_++;
        }

        k_sem_give(&delivered_);
    }

    bool WaitForDelivery() {
        return k_sem_take(&delivered_, K_MSEC(WAIT_MS)) == 0;
    }

    size_t Count() const {
        ScopedMutex guard(mutex_);

        return count_;
    }

    std::optional<float> At(size_t index) const {
        ScopedMutex guard(mutex_);

        if(index >= count_ || index >= CAPACITY)
            return std::nullopt;

        return values_[index];
    }

    void Clear() {
        ScopedMutex guard(mutex_);

        count_ = 0;
        k_sem_reset(&delivered_);
    }
};

// What an application does before anything binds: the bus registers the channel, the registry
// names it. Idempotent, so every suite that binds can call it.
inline void RegisterSensorEvents() {
    SensorEventBus::GetInstance();

    auto& channel = SensorEventsChannel::GetInstance();
    auto& registry = EventChannelRegistry::GetInstance();

    zassert_ok(registry.Register(EventChannelId::Sensors, channel));
    zassert_equal(registry.Find(EventChannelId::Sensors), &channel);
}

inline std::shared_ptr<Sensor> MakeSensor(std::string_view id) {
    auto sensor = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), id);
    sensor->configuration.type = SensorType::VIRTUAL_ANALOG;

    return sensor;
}

inline SensorReading MakeReading(const Sensor& sensor, std::optional<float> value) {
    SensorReading reading(&sensor);
    reading.source = ReadingSource::PROCESSING;
    reading.status = ReadingStatus::PROCESSED;
    reading.value = value;

    return reading;
}

// The widget and trigger path: a text selector, resolved through the registry.
inline AnySubscription Bind(const Sensor& sensor, const std::shared_ptr<Recorder>& recorder) {
    const EventBinding binding{ .channel = EventChannelId::Sensors,
        .event_type = std::to_underlying(SensorEventType::DataUpdated),
        .payload_key = std::to_underlying(SensorPayloadType::Value),
        .selector_key = std::to_underlying(SensorPayloadType::SensorId),
        .selector_value = std::pmr::string(sensor.id) };

    return SubscribeEventBinding(binding, [recorder](const EventData& value) {
        if(const auto* number = std::get_if<float>(&value))
            recorder->Record(*number);
    });
}

using Commit = std::pair<const Sensor*, std::optional<float>>;

// One tick over @p commits and the sentinel, then the wait for the sentinel's delivery.
inline bool Round(
    SensorEventsPublisher& publisher,
    SensorReadingsFrame& frame,
    std::initializer_list<Commit> commits,
    const Sensor& sentinel,
    Recorder& sentinel_recorder
) {

    if(!publisher.Pause())
        return false;

    for(const auto& [sensor, value] : commits)
        frame.AddOrUpdateReading(MakeReading(*sensor, value));

    frame.AddOrUpdateReading(MakeReading(sentinel, 0.0F));

    if(!publisher.Resume())
        return false;

    return sentinel_recorder.WaitForDelivery();
}

} // namespace sensor_events_test_support
