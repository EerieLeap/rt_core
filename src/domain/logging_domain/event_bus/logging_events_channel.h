#pragma once

#include <cstddef>

#include "subsys/event_bus/event_channel.h"

#include "logging_event_type.h"
#include "logging_payload_type.h"
#include "logging_status_reason.h"

namespace eerie_leap::domain::logging_domain::event_bus {

using eerie_leap::subsys::event_bus::EventChannel;

class LoggingEventsChannel : public EventChannel<LoggingEventType, LoggingPayloadType> {
private:
    static constexpr size_t K_MAX_QUEUED_EVENTS = 8;

    LoggingEventsChannel() :
        EventChannel("logging_events", LAST_LOGGING_EVENT_TYPE, LAST_LOGGING_PAYLOAD_TYPE, K_MAX_QUEUED_EVENTS) {}

public:
    static LoggingEventsChannel& GetInstance() {
        static LoggingEventsChannel channel;

        return channel;
    }
};

} // namespace eerie_leap::domain::logging_domain::event_bus
