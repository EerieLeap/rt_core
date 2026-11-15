#pragma once

#include <zephyr/drivers/can.h>

namespace eerie_leap::domain::can_monitor_domain::services {

/**
 * @brief Hears the state of a session's bus as the session's tap does.
 *
 * OnBusState() runs on the bus's receive thread, under the bus's lock, once for each change of state: it
 * must not block, take a lock another thread holds while it calls the bus, or allocate.
 */
class ICanBusStateListener {
public:
    virtual ~ICanBusStateListener() = default;

    virtual void OnBusState(can_state state) = 0;
};

} // namespace eerie_leap::domain::can_monitor_domain::services
