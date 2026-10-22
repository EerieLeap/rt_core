#pragma once

namespace eerie_leap::subsys::dfu {

/**
 * @brief The bootloader's view of the running image: whether a revert is pending and how to cancel it.
 *
 * McubootBootControl is the MCUboot implementation; tests use a fake.
 */
class IBootControl {
public:
    IBootControl() = default;
    IBootControl(const IBootControl&) = delete;
    IBootControl& operator=(const IBootControl&) = delete;
    IBootControl(IBootControl&&) = delete;
    IBootControl& operator=(IBootControl&&) = delete;
    virtual ~IBootControl() = default;

    /**
     * @brief True when the running image is permanent: no revert is pending.
     *
     * Reads the slot trailer (a few flash words), so it is cheap enough for every boot and every tick.
     */
    [[nodiscard]] virtual bool IsConfirmed() const = 0;

    /**
     * @brief Marks the running image confirmed, which writes the slot trailer.
     *
     * Succeeds without a write when the image is already confirmed.
     *
     * @return 0 on success, otherwise a negative errno.
     */
    [[nodiscard]] virtual int Confirm() = 0;
};

} // namespace eerie_leap::subsys::dfu
