#include <cstdint>
#include <string>
#include <variant>

#include <zephyr/ztest.h>

#include "domain/sensor_domain/event_bus/sensor_event_bus.h"
#include "subsys/event_bus/scoped_subscription.h"

using eerie_leap::domain::sensor_domain::event_bus::SensorEventBus;
using eerie_leap::domain::sensor_domain::event_bus::SensorEventsChannel;
using eerie_leap::domain::sensor_domain::event_bus::SensorEventType;
using eerie_leap::domain::sensor_domain::event_bus::SensorPayloadType;
using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::event_bus::CreateScopedSubscription;

namespace {

constexpr int DISPATCH_TIMEOUT_MS = 1000;

struct SensorProbe {
    k_sem delivered;
    uint32_t sensor_id = 0;
    float value = 0.0F;
    std::string handler_thread_name;

    SensorProbe() { k_sem_init(&delivered, 0, K_SEM_MAX_LIMIT); }

    void Record(const SensorEventsChannel::EventMessage& event) {
        const char* name = k_thread_name_get(k_current_get());
        handler_thread_name = name != nullptr ? std::string(name) : std::string();

        if(auto it = event.payload.find(SensorPayloadType::SensorId); it != event.payload.end())
            if(const auto* id = std::get_if<uint32_t>(&it->second))
                sensor_id = *id;

        if(auto it = event.payload.find(SensorPayloadType::Value); it != event.payload.end())
            if(const auto* reading = std::get_if<float>(&it->second))
                value = *reading;

        k_sem_give(&delivered);
    }

    bool WaitForDelivery() { return k_sem_take(&delivered, K_MSEC(DISPATCH_TIMEOUT_MS)) == 0; }
};

} // namespace

ZTEST_SUITE(sensor_event_bus, NULL, NULL, NULL, NULL, NULL);

ZTEST(sensor_event_bus, test_the_bus_registers_the_sensor_channel) {
    auto& bus = SensorEventBus::GetInstance();

    zassert_true(SensorEventsChannel::GetInstance().IsRegistered());
    zassert_equal(SensorEventsChannel::GetInstance().GetBus(), &bus);
}

ZTEST(sensor_event_bus, test_the_sensor_channel_runs_on_its_own_thread) {
    SensorEventBus::GetInstance();
    SensorProbe probe;

    AnySubscription subscription = CreateScopedSubscription(
        SensorEventsChannel::GetInstance(),
        SensorEventType::DataUpdated,
        [&probe](const SensorEventsChannel::EventMessage& event) { probe.Record(event); });
    zassert_not_null(subscription.get());

    SensorEventsChannel::GetInstance().PublishAsync({
        .type = SensorEventType::DataUpdated,
        .payload = {
            { SensorPayloadType::SensorId, 4242U },
            { SensorPayloadType::Value, 12.5F }
        },
        .source_id = "test"
    });

    zassert_true(probe.WaitForDelivery(), "the event was never dispatched");
    zassert_equal(probe.sensor_id, 4242U);
    zassert_within(probe.value, 12.5F, 0.001F);

    // Sensor traffic must not share a thread with the rest of the app.
    zassert_equal(probe.handler_thread_name, std::string("sensor_event_bus"));
}
