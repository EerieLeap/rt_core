#pragma once

#include "subsys/event_bus/event_bus.h"

#include "logging_events_channel.h"

namespace eerie_leap::domain::logging_domain::event_bus {

using eerie_leap::subsys::event_bus::EventBus;

// Delivers logging_events on its own thread. The channel drops every publish until a bus registers
// it, so an application that subscribes to it, and has no other bus carrying it, creates this one.
// The log writer publishes asynchronously: its subscribers never run on the writer thread, whose
// tick a synchronous Stop() would wait for.
class LoggingEventBus : public EventBus {
private:
    static constexpr int k_stack_size = 4096;

    LoggingEventBus() : EventBus("logging_event_bus", k_stack_size) {
        RegisterChannel(LoggingEventsChannel::GetInstance());
    }

public:
    static LoggingEventBus& GetInstance() {
        static LoggingEventBus bus;

        return bus;
    }
};

} // namespace eerie_leap::domain::logging_domain::event_bus
