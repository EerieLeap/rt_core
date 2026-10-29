#pragma once

#include <cstddef>

#include "domain/sensor_domain/models/sensor_limits.h"
#include "subsys/event_bus/event_channel.h"

#include "sensor_event_type.h"
#include "sensor_payload_type.h"

namespace eerie_leap::domain::sensor_domain::event_bus {

using eerie_leap::domain::sensor_domain::models::SensorLimits;
using eerie_leap::subsys::event_bus::EventChannel;

class SensorEventsChannel : public EventChannel<SensorEventType, SensorPayloadType> {
private:
    static constexpr size_t K_MAX_QUEUED_EVENTS = 128;

    // SensorEventsPublisher queues up to one event per sensor into an empty channel. Deeper than that,
    // the readings of a tick cannot fill the queue, so they never push out an event queued before them.
    static_assert(
        K_MAX_QUEUED_EVENTS > SensorLimits::MAX_COUNT,
        "sensor_events must be deeper than the events of one publisher tick"
    );

    SensorEventsChannel() :
        EventChannel("sensor_events", LAST_SENSOR_EVENT_TYPE, LAST_SENSOR_PAYLOAD_TYPE, K_MAX_QUEUED_EVENTS) {}

public:
    static SensorEventsChannel& GetInstance() {
        static SensorEventsChannel channel;

        return channel;
    }
};

} // namespace eerie_leap::domain::sensor_domain::event_bus
