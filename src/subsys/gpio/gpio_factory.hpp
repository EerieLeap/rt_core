#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "zephyr/drivers/gpio.h"

#include "composite_gpio.h"
#include "gpio.h"
#include "gpio_emulator.h"
#include "gpio_randomizer.h"
#include "gpio_simulator.h"

namespace eerie_leap::subsys::gpio {

class GpioFactory {
private:
    using DtGpioProvider = std::function<std::optional<std::vector<gpio_dt_spec>>&()>;
    DtGpioProvider dt_gpio_provider_;

public:
    explicit GpioFactory(DtGpioProvider dt_gpio_provider) :
        dt_gpio_provider_(std::move(dt_gpio_provider)) {}

    // The device-tree channels: emulated, else real. With CONFIG_EERIE_LEAP_GPIO_RANDOMIZER the
    // randomizer's channels follow them (hardware keeps its numbers), as a CompositeGpio, or alone
    // when there are none. A GpioSimulator stands in only when there is nothing else.
    std::shared_ptr<IGpio> Create() {
        std::shared_ptr<IGpio> hardware;

#ifdef CONFIG_GPIO_EMUL
        if(dt_gpio_provider_ != nullptr && dt_gpio_provider_().has_value())
            hardware = std::make_shared<GpioEmulator>(dt_gpio_provider_().value());
#endif

#ifdef CONFIG_GPIO
        if(hardware == nullptr && dt_gpio_provider_ != nullptr && dt_gpio_provider_().has_value())
            hardware = std::make_shared<Gpio>(dt_gpio_provider_().value());
#endif

#ifdef CONFIG_EERIE_LEAP_GPIO_RANDOMIZER
        auto randomizer = std::make_shared<GpioRandomizer>(GpioRandomizerConfig{
            .channel_count = CONFIG_EERIE_LEAP_GPIO_RANDOMIZER_CHANNELS,
            .hold_min_ms = CONFIG_EERIE_LEAP_GPIO_RANDOMIZER_HOLD_MIN_MS,
            .hold_max_ms = CONFIG_EERIE_LEAP_GPIO_RANDOMIZER_HOLD_MAX_MS,
        });

        if(hardware == nullptr)
            return randomizer;

        return std::make_shared<CompositeGpio>(std::vector<std::shared_ptr<IGpio>>{ hardware, randomizer });
#else
        if(hardware != nullptr)
            return hardware;

        return std::make_shared<GpioSimulator>();
#endif
    }
};

} // namespace eerie_leap::subsys::gpio
