#pragma once

#include <optional>
#include <vector>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include "zephyr/drivers/gpio.h"

#if DT_HAS_ALIAS(gpioc)
    #define GPIOC0_NODE DT_ALIAS(gpioc)
    #define GPIO_SPEC(node_id) GPIO_DT_SPEC_GET(node_id, gpios)
#endif

namespace eerie_leap::subsys::device_tree {

class DtGpio {
private:
    static std::optional<std::vector<gpio_dt_spec>> gpio_specs;

    DtGpio() = default;

public:
    static void Initialize();

    static std::optional<std::vector<gpio_dt_spec>>& Get() {
        return gpio_specs;
    }
};

} // namespace eerie_leap::subsys::device_tree
