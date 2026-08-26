#pragma once

#include <cstddef>
#include <memory>
#include <queue>

#include <zephyr/kernel.h>

#include "event.h"
#include "subscription.h"

namespace eerie_leap::subsys::event_bus {

template<concepts::EnumClassUint32 EventTypeEnum, concepts::EnumClassUint32 PayloadTypeEnum>
struct EventBusTask {
    static constexpr size_t k_max_queued_events = 64;

    k_sem* processing_semaphore;
    std::shared_ptr<SubscriberMap<EventTypeEnum, PayloadTypeEnum>> subscribers;
    k_mutex* subscribers_mutex;

    std::queue<Event<EventTypeEnum, PayloadTypeEnum>> event_queue;
    size_t dropped_events = 0;
    k_mutex queue_mutex;

    void (*dispatch_guard_before)() = nullptr;
    void (*dispatch_guard_after)() = nullptr;

    EventBusTask() {
        k_mutex_init(&queue_mutex);
    }
};

} // namespace eerie_leap::subsys::event_bus
