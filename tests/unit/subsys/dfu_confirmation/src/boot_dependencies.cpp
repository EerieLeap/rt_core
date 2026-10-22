#include <cerrno>

#include <zephyr/ztest.h>

#include "subsys/dfu/mcuboot_boot_control.h"
#include "subsys/dfu/zephyr_watchdog.h"

using eerie_leap::subsys::dfu::McubootBootControl;
using eerie_leap::subsys::dfu::ZephyrWatchdog;

// native_sim has neither an MCUboot image manager in this suite nor a watchdog0 alias: these cases pin down
// what the Zephyr implementations do then, which is what lets a build with EERIE_LEAP_DFU compile there.

ZTEST_SUITE(dfu_boot_dependencies, NULL, NULL, NULL, NULL, NULL);

ZTEST(dfu_boot_dependencies, test_boot_control_without_an_image_manager_is_confirmed_and_cannot_confirm) {
#ifdef CONFIG_MCUBOOT_IMG_MANAGER
    ztest_test_skip();
#else
    McubootBootControl boot_control;

    zassert_true(boot_control.IsConfirmed());
    zassert_equal(boot_control.Confirm(), -ENOTSUP);
#endif
}

ZTEST(dfu_boot_dependencies, test_watchdog_without_the_alias_is_unsupported) {
    if(ZephyrWatchdog::IsSupported())
        ztest_test_skip();

    ZephyrWatchdog watchdog;

    zassert_equal(watchdog.Install(8000), -ENOTSUP);
    zassert_equal(watchdog.Start(), -ENOTSUP);
    zassert_equal(watchdog.Feed(), -ENOTSUP);
    zassert_equal(watchdog.Disable(), -ENOTSUP);
}
