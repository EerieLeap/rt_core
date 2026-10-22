#include <cerrno>
#include <exception>
#include <stdexcept>
#include <utility>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#ifdef CONFIG_HWINFO
    #include <zephyr/drivers/hwinfo.h>
#endif

#include "subsys/dfu/image_identity.h"
#include "subsys/dfu/mcuboot_boot_control.h"
#include "subsys/dfu/zephyr_watchdog.h"

#include "dfu_controller.h"

// The MCUmgr work queue, which img_mgmt, the dfu group and every SMP request run on. Zephyr declares the
// getter only in its internal smp_internal.h (under CONFIG_SMP_CLIENT, which EERIE_LEAP_DFU selects for
// it), so it is declared here.
extern "C" k_work_q* smp_get_wq(void);

namespace eerie_leap::controllers {

LOG_MODULE_REGISTER(dfu_controller_logger);

using eerie_leap::subsys::dfu::ImageIdentity;
using eerie_leap::subsys::dfu::McubootBootControl;
using eerie_leap::subsys::dfu::ZephyrWatchdog;

namespace {

// The unit has a product ID (the system domain): the signing step requires it with EERIE_LEAP_DFU
// (cmake/sign_image.cmake), and the images carry the same one.
constexpr uint16_t PRODUCT_ID = CONFIG_EERIE_LEAP_PRODUCT_ID;

// What an image has to be built for to be accepted: this product and this board.
constexpr std::optional<ImageIdentity> OWN_IDENTITY = ImageIdentity::Create(PRODUCT_ID, CONFIG_BOARD_TARGET);
static_assert(OWN_IDENTITY.has_value(), "CONFIG_BOARD_TARGET must be printable ASCII of at most 116 characters.");

constexpr bool IS_BENCH_CRASH = IS_ENABLED(CONFIG_EERIE_LEAP_DFU_BENCH_FAULT_CRASH);
constexpr bool IS_BENCH_HANG = IS_ENABLED(CONFIG_EERIE_LEAP_DFU_BENCH_FAULT_HANG);
constexpr bool IS_BENCH_NO_REPORT = IS_ENABLED(CONFIG_EERIE_LEAP_DFU_BENCH_FAULT_NO_REPORT);

int64_t UptimeMs() {
    return k_uptime_get();
}

} // namespace

ImageConfirmationConfig DfuController::ConfigFromKconfig() {
    return {
        .self_confirm = IS_ENABLED(CONFIG_EERIE_LEAP_DFU_SELF_CONFIRM),
        .boot_deadline_ms = CONFIG_EERIE_LEAP_DFU_BOOT_DEADLINE_MS,
        .confirm_delay_ms = CONFIG_EERIE_LEAP_DFU_CONFIRM_DELAY_MS,
        .watchdog_timeout_ms = CONFIG_EERIE_LEAP_DFU_WATCHDOG_TIMEOUT_MS,
    };
}

DfuController::DfuController() :
    owned_boot_control_(std::make_unique<McubootBootControl>()),
    owned_watchdog_(ZephyrWatchdog::IsSupported() ? std::make_unique<ZephyrWatchdog>() : nullptr),
    boot_control_(*owned_boot_control_),
    watchdog_(owned_watchdog_.get()),
    config_(ConfigFromKconfig()) {}

DfuController::DfuController(IBootControl& boot_control, IWatchdog* watchdog, ImageConfirmationConfig config) :
    boot_control_(boot_control),
    watchdog_(watchdog),
    config_(config) {}

DfuController::~DfuController() {
    if(is_tick_initialized_) {
        k_work_sync sync{};
        k_work_cancel_delayable_sync(&tick_.work, &sync);
    }

    // The group and the guard unregister themselves; the probes wait for a handler that is running.
}

void DfuController::TickHandler(k_work* work) {
    const auto* item = CONTAINER_OF(k_work_delayable_from_work(work), TickItem, work);
    DfuController* controller = item->controller;

    controller->confirmation_->Tick();
    if(controller->confirmation_->NeedsTick())
        controller->ScheduleTick();
    else
        LOG_INF("Image confirmation done: state %u.", std::to_underlying(controller->confirmation_->GetState()));
}

void DfuController::ScheduleTick() {
    k_work_reschedule(&tick_.work, K_MSEC(confirmation_->GetTickIntervalMs()));
}

int DfuController::Initialize() {
    if(is_initialized_)
        return 0;

#ifdef CONFIG_HWINFO
    uint32_t reset_cause = 0;
    if(hwinfo_get_reset_cause(&reset_cause) == 0)
        reset_cause_ = reset_cause;
#endif
    LOG_INF("Reset cause 0x%08x.", reset_cause_);

    int result = 0;

    confirmation_ = std::make_unique<ImageConfirmation>(boot_control_, watchdog_, UptimeMs, config_);
    // Reads the slot trailer; arms the watchdog and the deadline only for an unconfirmed image. Never waits.
    if(const int init_result = confirmation_->Initialize(); init_result != 0) {
        LOG_ERR("Could not arm the watchdog (%d); a failed image will not be reset.", init_result);
        result = init_result;
    }

    const DfuState state = confirmation_->GetState();
    LOG_INF("Running image is %s.", state == DfuState::CONFIRMED ? "confirmed" : "unconfirmed (test boot)");

    try {
        // Cannot happen (static_assert above); checked so that the access below is.
        if(!OWN_IDENTITY.has_value())
            throw std::logic_error("The unit has no valid image identity.");

        upload_guard_.emplace(*OWN_IDENTITY);
        upload_guard_->Register();

        dfu_mgmt_group_ = std::make_unique<DfuMgmtGroup>(*this);
        dfu_mgmt_group_->Register();
    } catch(const std::exception& e) {
        LOG_ERR("Could not register the dfu SMP callbacks: %s", e.what());
        dfu_mgmt_group_.reset();
        upload_guard_.reset();
        result = -EEXIST;
    }

    if(state != DfuState::CONFIRMED) {
        mcumgr_probe_ = std::make_unique<WorkQueueProbe>("mcumgr", smp_get_wq());
        confirmation_->AddProbe(*mcumgr_probe_);

        tick_.controller = this;
        k_work_init_delayable(&tick_.work, TickHandler);
        is_tick_initialized_ = true;
        ScheduleTick();

        // After the watchdog is armed and the tick runs, so that the drills end in a reset.
        InjectBenchFault();
    }

    is_initialized_ = true;

    return result;
}

void DfuController::InjectBenchFault() {
    if constexpr(IS_BENCH_CRASH) {
        LOG_ERR("Bench fault: crashing the unconfirmed image.");
        k_panic();
    }

    if constexpr(IS_BENCH_HANG) {
        LOG_ERR("Bench fault: hanging the unconfirmed image.");
        // main() never gets on, so boot complete is never reported. The tick runs on the system work queue,
        // which outranks main(), and does not judge the probes while VERIFYING, so the watchdog is fed until
        // the boot deadline passes (60 s by default); the unit then resets one watchdog timeout later.
        while(true)
            k_busy_wait(1000);
    }

    if constexpr(IS_BENCH_NO_REPORT)
        LOG_WRN("Bench fault: the unconfirmed image will not report boot complete.");
}

void DfuController::RegisterLivenessProbe(const char* name, const std::shared_ptr<WorkQueueThread>& work_queue_thread) {
    if(work_queue_thread == nullptr || confirmation_ == nullptr || !confirmation_->NeedsTick())
        return;

    k_work_q* work_q = nullptr;
    try {
        work_q = work_queue_thread->GetWorkQueue();
    } catch(const std::exception& e) {
        LOG_WRN("The %s work queue is not probed: %s", name, e.what());
        return;
    }

    // Kept first: the probe is destroyed before the queue.
    probed_queues_.push_back(work_queue_thread);
    RegisterLivenessProbe(name, work_q);
}

void DfuController::RegisterLivenessProbe(const char* name, k_work_q* work_q) {
    if(confirmation_ == nullptr || !confirmation_->NeedsTick() || work_q == nullptr)
        return;

    // Owned before the machine sees it: a push that throws leaves nothing registered.
    probes_.push_back(std::make_unique<WorkQueueProbe>(name, work_q));
    confirmation_->AddProbe(*probes_.back());
}

void DfuController::ReportBootComplete() {
    if(confirmation_ == nullptr)
        return;

    if constexpr(IS_BENCH_NO_REPORT) {
        if(!confirmation_->IsConfirmed()) {
            LOG_WRN("Bench fault: boot complete is not reported.");
            return;
        }
    }

    confirmation_->ReportBootComplete();
}

DfuState DfuController::GetState() const {
    return confirmation_ != nullptr ? confirmation_->GetState() : DfuState::CONFIRMED;
}

DfuStatus DfuController::GetStatus() const {
    DfuStatus status;
    status.reset_cause = reset_cause_;

    if(confirmation_ == nullptr)
        return status;

    status.state = confirmation_->GetState();
    status.confirmed = status.state == DfuState::CONFIRMED;
    status.confirm_in_ms = confirmation_->GetConfirmInMs();

    return status;
}

} // namespace eerie_leap::controllers
