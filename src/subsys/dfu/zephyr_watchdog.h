#pragma once

#include "i_watchdog.h"

struct device;

namespace eerie_leap::subsys::dfu {

/**
 * @brief IWatchdog on the `watchdog0` alias (the MWDT of timer group 0 on the ESP32 boards).
 *
 * The timeout is passed to Zephyr as is. wdt_esp32 programs two stages and counts 0.5 ms (S3) or
 * 1 ms (P4) ticks, so the time the unit really takes to reset differs from the configured value. It is
 * measured on the bench (FIRMWARE_UPDATE_PLAN 3.7) and not compensated here yet.
 *
 * A build without CONFIG_WATCHDOG or the alias still compiles: IsSupported() is false and every call
 * returns -ENOTSUP.
 */
class ZephyrWatchdog final : public IWatchdog {
private:
    // Only a build with watchdog0 uses them.
    [[maybe_unused]] const device* device_ = nullptr;
    [[maybe_unused]] int channel_id_ = -1;

public:
    ZephyrWatchdog();

    /** @brief True when this build has a watchdog0 alias and CONFIG_WATCHDOG. */
    [[nodiscard]] static bool IsSupported();

    [[nodiscard]] int Install(uint32_t timeout_ms) override;
    [[nodiscard]] int Start() override;
    [[nodiscard]] int Feed() override;
    [[nodiscard]] int Disable() override;
};

} // namespace eerie_leap::subsys::dfu
