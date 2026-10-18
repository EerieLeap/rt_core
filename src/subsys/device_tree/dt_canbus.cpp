#include <zephyr/logging/log.h>

#include "dt_logger.h"

#include "dt_canbus.h"

namespace eerie_leap::subsys::device_tree {

LOG_MODULE_DECLARE(dt_logger);

std::unordered_map<uint8_t, const device*> DtCanbus::canbus_devs;

void DtCanbus::Initialize() {
#ifdef CAN0_NODE
    const auto* can0_dev = DEVICE_DT_GET(CAN0_NODE);
    canbus_devs.emplace(0, can0_dev);
    LOG_INF("CAN0 initialized.");
#endif

#ifdef CAN1_NODE
    auto can1_dev = DEVICE_DT_GET(CAN1_NODE);
    canbus_devs.emplace(1, can1_dev);
    LOG_INF("CAN1 initialized.");
#endif
}

const device* DtCanbus::Get(uint8_t bus_channel) {
    if(!canbus_devs.contains(bus_channel))
        return nullptr;

    return canbus_devs.at(bus_channel);
}

} // namespace eerie_leap::subsys::device_tree
