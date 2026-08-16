#pragma once

#include <zephyr/random/random.h>

#include "i_gpio.h"

namespace eerie_leap::subsys::gpio {

class GpioSimulator : public IGpio {
public:
    GpioSimulator() = default;
    ~GpioSimulator() override = default;

    int Initialize() override;
    bool ReadChannel(int channel) override;
    int GetChannelCount() override;

    int RegisterChannelChangedHandler(int channel, GpioEdge edge, GpioChannelHandler handler) override;
    bool RemoveChannelChangedHandler(int channel, int handler_id) override;
};

}  // namespace eerie_leap::subsys::gpio
