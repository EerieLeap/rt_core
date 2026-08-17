#include <stdexcept>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>

#include "gpio_simulator.h"

namespace eerie_leap::subsys::gpio {

LOG_MODULE_REGISTER(gpio_simulator_logger);

int GpioSimulator::Initialize() {
    LOG_INF("Gpio Simulator initialization started.");

    LOG_INF("Gpio Simulator initialized successfully.");

    return 0;
}

bool GpioSimulator::ReadChannel(int channel) {
    if(channel < 0 || channel > 31)
        throw std::invalid_argument("Gpio channel out of range.");

    uint32_t raw = sys_rand32_get();
    float random_value = (raw / static_cast<float>(UINT32_MAX)) * 3.3F;

    return random_value > 1.65F;
}

int GpioSimulator::GetChannelCount() {
    return 32;
}

int GpioSimulator::RegisterChannelChangedHandler(int channel, GpioEdge edge, GpioChannelHandler handler) {
    ARG_UNUSED(channel);
    ARG_UNUSED(edge);
    ARG_UNUSED(handler);

    LOG_WRN("Gpio Simulator does not generate channel interrupts.");

    return ERR_NOT_SUPPORTED;
}

bool GpioSimulator::RemoveChannelChangedHandler(int channel, int handler_id) {
    ARG_UNUSED(channel);
    ARG_UNUSED(handler_id);

    return false;
}

}  // namespace eerie_leap::subsys::gpio
