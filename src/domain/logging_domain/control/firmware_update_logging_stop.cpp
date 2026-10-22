#include <stdexcept>
#include <utility>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#ifdef CONFIG_MCUMGR_MGMT_NOTIFICATION_HOOKS
// clang-format off
// The group headers use the macros of callbacks.h and include nothing, so it must come first; the
// include sort would put them before it. The event IDs are plain enumerators: the img header needs
// no img_mgmt build.
    #include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
    #include <zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt_callbacks.h>
    #ifdef CONFIG_MCUMGR_GRP_OS_RESET_HOOK
        #include <zephyr/mgmt/mcumgr/grp/os_mgmt/os_mgmt_callbacks.h>
    #endif
// clang-format on
#endif

#include "subsys/threading/scoped_mutex.h"

#include "firmware_update_logging_stop.h"

namespace eerie_leap::domain::logging_domain::control {

using eerie_leap::subsys::threading::ScopedMutex;

LOG_MODULE_REGISTER(firmware_update_logging_stop_logger);

namespace {

// The slot the hooks find their instance through, and the callbacks MCUmgr links.
struct Hooks {
    k_mutex lock{};
    const FirmwareUpdateLoggingStop* owner = nullptr;
#ifdef CONFIG_MCUMGR_MGMT_NOTIFICATION_HOOKS
    mgmt_callback upload_started{};
    #ifdef CONFIG_MCUMGR_GRP_OS_RESET_HOOK
    mgmt_callback reset{};
    #endif
#endif

    Hooks() {
        k_mutex_init(&lock);
    }
};

Hooks& GetHooks() {
    static Hooks hooks;

    return hooks;
}

#ifdef CONFIG_MCUMGR_MGMT_NOTIFICATION_HOOKS

// The lock is held across the hook, so Unregister() waits for it. The hooks always let the request go
// ahead: a stop that did not finish is not a reason to refuse a reset or an upload.
mgmt_cb_return OnUploadStartedHook(
    uint32_t /*event*/,
    mgmt_cb_return /*prev_status*/,
    int32_t* /*rc*/,
    uint16_t* /*group*/,
    bool* /*abort_more*/,
    void* /*data*/,
    size_t /*data_size*/
) {
    auto& hooks = GetHooks();
    const ScopedMutex lock(hooks.lock);

    if(hooks.owner != nullptr)
        hooks.owner->OnUploadStarted();

    return MGMT_CB_OK;
}

    #ifdef CONFIG_MCUMGR_GRP_OS_RESET_HOOK
mgmt_cb_return OnResetHook(
    uint32_t /*event*/,
    mgmt_cb_return /*prev_status*/,
    int32_t* /*rc*/,
    uint16_t* /*group*/,
    bool* /*abort_more*/,
    void* /*data*/,
    size_t /*data_size*/
) {
    auto& hooks = GetHooks();
    const ScopedMutex lock(hooks.lock);

    if(hooks.owner != nullptr)
        hooks.owner->OnReset();

    return MGMT_CB_OK;
}
    #endif

#endif

} // namespace

FirmwareUpdateLoggingStop::FirmwareUpdateLoggingStop(StopRequest request_stop, StopWait wait_for_stop) :
    request_stop_(std::move(request_stop)),
    wait_for_stop_(std::move(wait_for_stop)) {

    if(!request_stop_ || !wait_for_stop_)
        throw std::invalid_argument("The firmware update logging stop needs both of its functions.");
}

FirmwareUpdateLoggingStop::~FirmwareUpdateLoggingStop() {
    Unregister();
}

bool FirmwareUpdateLoggingStop::Register() {
#ifdef CONFIG_MCUMGR_MGMT_NOTIFICATION_HOOKS
    auto& hooks = GetHooks();
    const ScopedMutex lock(hooks.lock);

    if(hooks.owner == this)
        return true;

    if(hooks.owner != nullptr) {
        LOG_ERR("Another firmware update logging stop holds the MCUmgr hooks.");
        return false;
    }

    hooks.upload_started.callback = OnUploadStartedHook;
    hooks.upload_started.event_id = MGMT_EVT_OP_IMG_MGMT_DFU_STARTED;
    mgmt_callback_register(&hooks.upload_started);

    #ifdef CONFIG_MCUMGR_GRP_OS_RESET_HOOK
    hooks.reset.callback = OnResetHook;
    hooks.reset.event_id = MGMT_EVT_OP_OS_MGMT_RESET;
    mgmt_callback_register(&hooks.reset);
    #else
    LOG_WRN("Without MCUMGR_GRP_OS_RESET_HOOK the log is not finalized before an os reset.");
    #endif

    hooks.owner = this;

    return true;
#else
    LOG_DBG("Without MCUMGR_MGMT_NOTIFICATION_HOOKS a firmware update does not stop logging.");

    return false;
#endif
}

void FirmwareUpdateLoggingStop::Unregister() {
#ifdef CONFIG_MCUMGR_MGMT_NOTIFICATION_HOOKS
    auto& hooks = GetHooks();
    const ScopedMutex lock(hooks.lock);

    if(hooks.owner != this)
        return;

    mgmt_callback_unregister(&hooks.upload_started);
    #ifdef CONFIG_MCUMGR_GRP_OS_RESET_HOOK
    mgmt_callback_unregister(&hooks.reset);
    #endif

    hooks.owner = nullptr;
#endif
}

bool FirmwareUpdateLoggingStop::IsRegistered() const {
    auto& hooks = GetHooks();
    const ScopedMutex lock(hooks.lock);

    return hooks.owner == this;
}

void FirmwareUpdateLoggingStop::OnUploadStarted() const {
    LOG_DBG("A firmware upload started: logging stops.");

    request_stop_();
}

void FirmwareUpdateLoggingStop::OnReset() const {
    LOG_DBG("A reset was requested: logging stops first.");

    request_stop_();

    if(!wait_for_stop_(RESET_WAIT_MS))
        LOG_WRN(
            "The log writer did not confirm its stop within %d ms; the reset goes ahead.",
            static_cast<int>(RESET_WAIT_MS)
        );
}

} // namespace eerie_leap::domain::logging_domain::control
