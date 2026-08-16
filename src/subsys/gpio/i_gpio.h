#pragma once

#include <functional>

#include "gpio_edge.h"

namespace eerie_leap::subsys::gpio {

// Handlers are dispatched from a bottom half, not from the interrupt itself,
// so they may block, but they share the system work queue with the rest of the
// system: offload anything long running to a dedicated work queue.
using GpioChannelHandler = std::function<void(int channel, bool state)>;

class IGpio {
public:
    static constexpr int ERR_NOT_INITIALIZED = -1;
    static constexpr int ERR_INVALID_ARGUMENT = -2;
    static constexpr int ERR_INTERRUPT_REJECTED = -3;
    static constexpr int ERR_NOT_SUPPORTED = -4;

    virtual ~IGpio() = default;

    virtual int Initialize() = 0;
    virtual bool ReadChannel(int channel) = 0;
    virtual int GetChannelCount() = 0;

    // Returns a positive handler id, or a negative ERR_* value.
    virtual int RegisterChannelChangedHandler(int channel, GpioEdge edge, GpioChannelHandler handler) = 0;
    virtual bool RemoveChannelChangedHandler(int channel, int handler_id) = 0;
};

}  // namespace eerie_leap::subsys::gpio
