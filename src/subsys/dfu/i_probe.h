#pragma once

namespace eerie_leap::subsys::dfu {

/**
 * @brief A liveness probe: proves that something the unit depends on (a work queue) still runs.
 *
 * ImageConfirmation::Tick() calls Check() on every probe and then Arm(). A probe whose Check() is false
 * did not run during the whole interval, so the unit is not healthy. Arm() and Check() must not block.
 */
class IProbe {
public:
    IProbe() = default;
    IProbe(const IProbe&) = delete;
    IProbe& operator=(const IProbe&) = delete;
    IProbe(IProbe&&) = delete;
    IProbe& operator=(IProbe&&) = delete;
    virtual ~IProbe() = default;

    /** @brief Clears the ran flag and posts the probe to what it watches. */
    virtual void Arm() = 0;

    /** @brief True when the probe ran since the last Arm(). */
    [[nodiscard]] virtual bool Check() const = 0;

    /** @brief Names the probe in the log; points to storage that outlives the probe. */
    [[nodiscard]] virtual const char* GetName() const = 0;
};

} // namespace eerie_leap::subsys::dfu
