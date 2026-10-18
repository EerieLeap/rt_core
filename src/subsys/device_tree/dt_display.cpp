#include <zephyr/logging/log.h>

#include "dt_logger.h"

#include "dt_display.h"

namespace eerie_leap::subsys::device_tree {

LOG_MODULE_DECLARE(dt_logger);

const device* DtDisplay::display_dev = nullptr;

void DtDisplay::Initialize() {
#if defined(CONFIG_DISPLAY) && DT_HAS_CHOSEN(zephyr_display)
    display_dev = DEVICE_DT_GET(DISPLAY_NODE);
    LOG_INF("Display initialized.");
#endif
}

} // namespace eerie_leap::subsys::device_tree
