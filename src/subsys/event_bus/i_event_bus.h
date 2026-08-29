#pragma once

namespace eerie_leap::subsys::event_bus {

class IEventBus {
public:
    virtual ~IEventBus() = default;

    // Asks the bus worker to drain the queues of every channel registered with it.
    virtual void Wake() = 0;
};

} // namespace eerie_leap::subsys::event_bus
