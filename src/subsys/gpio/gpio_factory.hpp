#pragma once

#include <memory>
#include <utility>

#include "zephyr/drivers/gpio.h"

#include "gpio.h"
#include "gpio_emulator.h"
#include "gpio_simulator.h"

namespace eerie_leap::subsys::gpio {

class GpioFactory {
private:
    using DtGpioProvider = std::function<std::optional<std::vector<gpio_dt_spec>>&()>;
    DtGpioProvider dt_gpio_provider_;

public:
    GpioFactory(DtGpioProvider dt_gpio_provider) :
        dt_gpio_provider_(std::move(dt_gpio_provider)) {}

    std::shared_ptr<IGpio> Create() {
#ifdef CONFIG_GPIO_EMUL
        if(dt_gpio_provider_ != nullptr && dt_gpio_provider_().has_value())
            return std::make_shared<GpioEmulator>(dt_gpio_provider_().value());
#endif

#ifdef CONFIG_GPIO
        if(dt_gpio_provider_ != nullptr && dt_gpio_provider_().has_value())
            return std::make_shared<Gpio>(dt_gpio_provider_().value());
#endif

        return std::make_shared<GpioSimulator>();
    }
};

} // namespace eerie_leap::subsys::gpio
