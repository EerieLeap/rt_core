#pragma once

#include <functional>
#include <memory>

#include "subsys/bluetooth/i_ble_notifier.h"

namespace eerie_leap::domain::can_monitor_domain::services {

using eerie_leap::subsys::bluetooth::IBleNotifier;

/**
 * @brief What the CAN monitor takes from a transport. Every member may be empty (a missing notifier is
 * a link nobody listens on); tests fake them.
 *
 * The controller fills it from the BLE service and routes the completion and disconnect events to
 * CanMonitorService::OnNotificationSent() and OnDisconnected().
 */
struct CanMonitorLink {
    /// The `bus` characteristic.
    std::shared_ptr<IBleNotifier> notifier;
    /// Holds the fast link parameters; one call for each session opened, balanced by release_fast.
    std::function<void()> hold_fast;
    std::function<void()> release_fast;
    /// True while an SMP response is being notified: the stream waits. Called from the sender thread.
    std::function<bool()> is_smp_busy;
};

} // namespace eerie_leap::domain::can_monitor_domain::services
