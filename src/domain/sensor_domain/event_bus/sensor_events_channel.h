#pragma once

#include <cstddef>

#include "subsys/event_bus/event_channel.h"

#include "sensor_event_type.h"
#include "sensor_payload_type.h"

namespace eerie_leap::domain::sensor_domain::event_bus {

using eerie_leap::subsys::event_bus::EventChannel;

class SensorEventsChannel : public EventChannel<SensorEventType, SensorPayloadType> {
private:
    static constexpr size_t K_MAX_QUEUED_EVENTS = 128;

    SensorEventsChannel() :
        EventChannel("sensor_events", K_MAX_QUEUED_EVENTS) {}

public:
    static SensorEventsChannel& GetInstance() {
        static SensorEventsChannel channel;

        return channel;
    }
};

} // namespace eerie_leap::domain::sensor_domain::event_bus
