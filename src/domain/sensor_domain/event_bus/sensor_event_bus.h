#pragma once

#include "subsys/event_bus/event_bus.h"

#include "sensor_events_channel.h"

namespace eerie_leap::domain::sensor_domain::event_bus {

using eerie_leap::subsys::event_bus::EventBus;

class SensorEventBus : public EventBus {
private:
    static constexpr int k_stack_size = 4096;

    SensorEventBus() : EventBus("sensor_event_bus", k_stack_size) {
        RegisterChannel(SensorEventsChannel::GetInstance());
    }

public:
    static SensorEventBus& GetInstance() {
        static SensorEventBus bus;

        return bus;
    }
};

} // namespace eerie_leap::domain::sensor_domain::event_bus
