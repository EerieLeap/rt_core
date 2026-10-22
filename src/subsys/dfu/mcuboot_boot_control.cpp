#include <cerrno>

#include "mcuboot_boot_control.h"

#ifdef CONFIG_MCUBOOT_IMG_MANAGER
    #include <zephyr/dfu/mcuboot.h>
#endif

namespace eerie_leap::subsys::dfu {

bool McubootBootControl::IsConfirmed() const {
#ifdef CONFIG_MCUBOOT_IMG_MANAGER
    return boot_is_img_confirmed();
#else
    return true;
#endif
}

int McubootBootControl::Confirm() {
#ifdef CONFIG_MCUBOOT_IMG_MANAGER
    return boot_write_img_confirmed();
#else
    return -ENOTSUP;
#endif
}

} // namespace eerie_leap::subsys::dfu
