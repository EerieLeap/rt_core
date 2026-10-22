#pragma once

#include <cstdint>
#include <functional>

namespace eerie_leap::domain::logging_domain::control {

// Ends the logging of a unit that writes logs when a firmware update comes (D8 of the firmware update
// plan), on MCUmgr's notifications:
// - an image upload starts (MGMT_EVT_OP_IMG_MGMT_DFU_STARTED, which the img group sends with
//   CONFIG_MCUMGR_GRP_IMG_STATUS_HOOKS): the writer stops with LoggingStatusReason::FirmwareUpdate. The
//   flash stalls of an upload would distort the log, and the reset that ends it would cut the file.
//   Nothing starts logging again; a start by hand or a trigger still does;
// - an `os reset` arrives (MGMT_EVT_OP_OS_MGMT_RESET, CONFIG_MCUMGR_GRP_OS_RESET_HOOK): the writer stops
//   as above and the hook waits, at most RESET_WAIT_MS, for the stop to finish, so the file is
//   finalized before the reboot. Then it lets the reset go ahead, also when the wait ran out (logged).
//
// The hooks run on the MCUmgr work queue, which has a small stack and takes no allocation or exception:
// they call the two functions below and nothing else. A unit that writes no logs builds none (the gauge's
// remote role).
//
// An MCUmgr callback carries no context, so the hooks reach their instance through a slot of this
// file: one instance is registered at a time, and Register() of a second one fails. Register() and
// Unregister() change MCUmgr's callback list, which Zephyr does not lock: call them at start-up and
// teardown, not while a request can arrive. Unregister() waits for a hook in progress.
class FirmwareUpdateLoggingStop {
public:
    // Orders the stop with LoggingStatusReason::FirmwareUpdate and returns at once.
    using StopRequest = std::function<void()>;
    // Waits for the stop ordered so far to be finished, at most the given milliseconds; whether it was.
    using StopWait = std::function<bool(int32_t timeout_ms)>;

    // How long the reset hook waits for the writer to finalize its file.
    static constexpr int32_t RESET_WAIT_MS = 2000;

    /**
     * Throws std::invalid_argument for an empty function. Registers nothing yet.
     */
    FirmwareUpdateLoggingStop(StopRequest request_stop, StopWait wait_for_stop);

    // Unregisters the hooks.
    ~FirmwareUpdateLoggingStop();

    // The hooks refer to it.
    FirmwareUpdateLoggingStop(const FirmwareUpdateLoggingStop&) = delete;
    FirmwareUpdateLoggingStop& operator=(const FirmwareUpdateLoggingStop&) = delete;
    FirmwareUpdateLoggingStop(FirmwareUpdateLoggingStop&&) = delete;
    FirmwareUpdateLoggingStop& operator=(FirmwareUpdateLoggingStop&&) = delete;

    /**
     * Registers the hooks the build has: the upload start with CONFIG_MCUMGR_MGMT_NOTIFICATION_HOOKS, the
     * reset with CONFIG_MCUMGR_GRP_OS_RESET_HOOK. False when it has none (registering for an event
     * nothing sends would be harmless; there is just nothing to register) or another instance holds the
     * slot. True when already registered.
     */
    bool Register();

    // Withdraws the hooks and waits for one in progress, which it must not be called from. Safe when
    // nothing was registered.
    void Unregister();

    [[nodiscard]] bool IsRegistered() const;

    // What the hooks do, on the MCUmgr work queue.
    void OnUploadStarted() const;
    void OnReset() const;

private:
    StopRequest request_stop_;
    StopWait wait_for_stop_;
};

} // namespace eerie_leap::domain::logging_domain::control
