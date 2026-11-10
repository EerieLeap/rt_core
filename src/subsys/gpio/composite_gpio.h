#pragma once

#include <memory>
#include <vector>

#include <zephyr/kernel.h>

#include "i_gpio.h"

namespace eerie_leap::subsys::gpio {

// Several GPIO devices as one: channels are concatenated in device order, so the first device keeps
// its numbers and later devices follow it (hardware first, then a randomizer).
class CompositeGpio : public IGpio {
private:
    struct Registration {
        int id;
        int channel; // Global.
        int device;
        int inner_id;
    };

    struct Resolved {
        int device;
        int local_channel;
    };

    std::vector<std::shared_ptr<IGpio>> devices_;
    std::vector<Registration> registrations_;

    mutable k_mutex lock_{};
    int next_handler_id_ = 1;

    // Offset of a device's first channel.
    [[nodiscard]] int OffsetOf(int device) const;
    [[nodiscard]] bool Resolve(int channel, Resolved& resolved) const;

public:
    explicit CompositeGpio(std::vector<std::shared_ptr<IGpio>> devices);

    // Stops at the first device that fails and returns its code.
    int Initialize() override;
    bool ReadChannel(int channel) override;
    int GetChannelCount() override;

    // A handler is called with the global channel number.
    int RegisterChannelChangedHandler(int channel, GpioEdge edge, GpioChannelHandler handler) override;
    bool RemoveChannelChangedHandler(int channel, int handler_id) override;
};

} // namespace eerie_leap::subsys::gpio
