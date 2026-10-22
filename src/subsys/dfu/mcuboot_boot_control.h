#pragma once

#include "i_boot_control.h"

namespace eerie_leap::subsys::dfu {

/**
 * @brief IBootControl over zephyr/dfu/mcuboot.h (`boot_is_img_confirmed`, `boot_write_img_confirmed`).
 *
 * Without CONFIG_MCUBOOT_IMG_MANAGER (a build with no MCUboot image manager) there is no slot trailer
 * to read: the image counts as confirmed and Confirm() returns -ENOTSUP.
 */
class McubootBootControl final : public IBootControl {
public:
    [[nodiscard]] bool IsConfirmed() const override;
    [[nodiscard]] int Confirm() override;
};

} // namespace eerie_leap::subsys::dfu
