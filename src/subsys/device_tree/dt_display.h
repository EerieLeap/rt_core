#pragma once

#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#if DT_HAS_CHOSEN(zephyr_display)
    #define DISPLAY_NODE DT_CHOSEN(zephyr_display)
#endif

namespace eerie_leap::subsys::device_tree {

class DtDisplay {
private:
    static const device* display_dev;

    DtDisplay() = default;

public:
    static void Initialize();

    static const device* Get() {
        return display_dev;
    }
};

} // namespace eerie_leap::subsys::device_tree
