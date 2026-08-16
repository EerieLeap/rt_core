#pragma once

#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>

#include "gpio.h"

namespace eerie_leap::subsys::gpio {

#define GPIOC0_NODE DT_ALIAS(gpioc)

class GpioEmulator : public Gpio {
public:
    explicit GpioEmulator(std::vector<gpio_dt_spec> gpio_specs) : Gpio(std::move(gpio_specs)) {}
    ~GpioEmulator() override = default;

    int Initialize() override;
    bool ReadChannel(int channel) override;
};

}  // namespace eerie_leap::subsys::gpio
