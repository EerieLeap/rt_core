#pragma once

#include <atomic>
#include <cstddef>

#include <zephyr/kernel.h>

#include "i_event_channel.h"

namespace eerie_leap::subsys::event_bus {

// Borrowed from the bus that owns the work queue task carrying this.
struct EventBusTask {
    k_sem* processing_semaphore;

    EventChannelSlots* channels;
    std::atomic<size_t>* channel_count;
};

} // namespace eerie_leap::subsys::event_bus
