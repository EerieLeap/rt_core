#pragma once

#include <cstdint>

namespace eerie_leap::subsys::dfu {

/**
 * @brief One hardware watchdog that resets the SoC: ImageConfirmation arms it while an image is unconfirmed.
 *
 * Every call returns 0 on success and a negative errno otherwise.
 */
class IWatchdog {
public:
    IWatchdog() = default;
    IWatchdog(const IWatchdog&) = delete;
    IWatchdog& operator=(const IWatchdog&) = delete;
    IWatchdog(IWatchdog&&) = delete;
    IWatchdog& operator=(IWatchdog&&) = delete;
    virtual ~IWatchdog() = default;

    /** @brief Configures a reset of the whole SoC after @p timeout_ms without a Feed(); Start() arms it. */
    [[nodiscard]] virtual int Install(uint32_t timeout_ms) = 0;

    /** @brief Arms the installed timeout; the countdown starts here. */
    [[nodiscard]] virtual int Start() = 0;

    /** @brief Restarts the countdown. */
    [[nodiscard]] virtual int Feed() = 0;

    /** @brief Stops the watchdog, so it no longer resets the unit. */
    [[nodiscard]] virtual int Disable() = 0;
};

} // namespace eerie_leap::subsys::dfu
