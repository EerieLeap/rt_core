#include <cerrno>

#include <zephyr/devicetree.h>

#include "zephyr_watchdog.h"

#if defined(CONFIG_WATCHDOG) && DT_NODE_HAS_STATUS(DT_ALIAS(watchdog0), okay)
    #include <zephyr/device.h>
    #include <zephyr/drivers/watchdog.h>

    #define EERIE_LEAP_DFU_WATCHDOG_NODE DT_ALIAS(watchdog0)
#endif

namespace eerie_leap::subsys::dfu {

#ifdef EERIE_LEAP_DFU_WATCHDOG_NODE

ZephyrWatchdog::ZephyrWatchdog() :
    device_(DEVICE_DT_GET(EERIE_LEAP_DFU_WATCHDOG_NODE)) {}

bool ZephyrWatchdog::IsSupported() {
    return true;
}

int ZephyrWatchdog::Install(uint32_t timeout_ms) {
    if(!device_is_ready(device_))
        return -ENODEV;

    wdt_timeout_cfg config{};
    config.window.min = 0;
    config.window.max = timeout_ms;
    config.callback = nullptr;
    config.flags = WDT_FLAG_RESET_SOC;

    // The channel id (0 on wdt_esp32), or a negative errno.
    const int channel_id = wdt_install_timeout(device_, &config);
    if(channel_id < 0)
        return channel_id;

    channel_id_ = channel_id;

    return 0;
}

int ZephyrWatchdog::Start() {
    if(channel_id_ < 0)
        return -EINVAL;

    // No pause options: wdt_esp32 ignores them.
    return wdt_setup(device_, 0);
}

int ZephyrWatchdog::Feed() {
    if(channel_id_ < 0)
        return -EINVAL;

    return wdt_feed(device_, channel_id_);
}

int ZephyrWatchdog::Disable() {
    if(channel_id_ < 0)
        return -EINVAL;

    const int result = wdt_disable(device_);
    if(result == 0)
        channel_id_ = -1;

    return result;
}

#else

ZephyrWatchdog::ZephyrWatchdog() = default;

bool ZephyrWatchdog::IsSupported() {
    return false;
}

int ZephyrWatchdog::Install(uint32_t) {
    return -ENOTSUP;
}

int ZephyrWatchdog::Start() {
    return -ENOTSUP;
}

int ZephyrWatchdog::Feed() {
    return -ENOTSUP;
}

int ZephyrWatchdog::Disable() {
    return -ENOTSUP;
}

#endif

} // namespace eerie_leap::subsys::dfu
