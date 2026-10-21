#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <zephyr/kernel.h>

#include "subsys/dfu/dfu_status.h"
#include "subsys/dfu/i_boot_control.h"
#include "subsys/dfu/i_watchdog.h"
#include "subsys/dfu/image_confirmation.h"
#include "subsys/dfu/image_upload_guard.h"
#include "subsys/dfu/smp/dfu_mgmt_group.h"
#include "subsys/dfu/work_queue_probe.h"

namespace eerie_leap::controllers {

using eerie_leap::subsys::dfu::DfuState;
using eerie_leap::subsys::dfu::DfuStatus;
using eerie_leap::subsys::dfu::IBootControl;
using eerie_leap::subsys::dfu::IDfuStatusSource;
using eerie_leap::subsys::dfu::ImageConfirmation;
using eerie_leap::subsys::dfu::ImageConfirmationConfig;
using eerie_leap::subsys::dfu::ImageUploadGuard;
using eerie_leap::subsys::dfu::IWatchdog;
using eerie_leap::subsys::dfu::WorkQueueProbe;
using eerie_leap::subsys::dfu::smp::DfuMgmtGroup;

/**
 * @brief Firmware update of a unit: the identity check of uploads, the `dfu` SMP group and the confirmation
 *        of a new image (docs/dfu.md).
 *
 * `main()` constructs it first and calls Initialize() before anything else comes up. A confirmed image
 * (every normal boot) costs one read of the slot trailer: no watchdog, no work item. An unconfirmed one
 * (a test boot after a swap) arms the hardware watchdog and a boot deadline, and is confirmed in the
 * background once the application calls ReportBootComplete() and the liveness probes have kept passing for
 * the confirmation delay; nothing waits for it. A hang, a crash or a probe that stops running ends in a
 * watchdog reset, and MCUboot then reverts to the previous image.
 *
 * Initialize() registers the upload guard and the `dfu` group with MCUmgr, so at most one controller
 * exists at a time.
 */
class DfuController final : public IDfuStatusSource {
private:
    // Standard layout, so that the handler can find the controller from the k_work with CONTAINER_OF.
    struct TickItem {
        k_work_delayable work{};
        DfuController* controller = nullptr;
    };

    std::unique_ptr<IBootControl> owned_boot_control_;
    std::unique_ptr<IWatchdog> owned_watchdog_;
    IBootControl& boot_control_;
    // Null on a board without a watchdog.
    IWatchdog* watchdog_;
    ImageConfirmationConfig config_;

    std::unique_ptr<ImageConfirmation> confirmation_;
    std::optional<ImageUploadGuard> upload_guard_;
    std::unique_ptr<DfuMgmtGroup> dfu_mgmt_group_;
    std::unique_ptr<WorkQueueProbe> mcumgr_probe_;
    std::vector<std::unique_ptr<WorkQueueProbe>> probes_;

    TickItem tick_{};
    bool is_tick_initialized_ = false;
    bool is_initialized_ = false;

    /// The hwinfo reset cause mask of this boot, read once by Initialize().
    uint32_t reset_cause_ = 0;

    static void TickHandler(k_work* work);
    void ScheduleTick();
    /// Fails the boot on purpose when the build asks for it (EERIE_LEAP_DFU_BENCH_FAULT_*) and the image is unconfirmed.
    void InjectBenchFault() const;

    static ImageConfirmationConfig ConfigFromKconfig();

public:
    /**
     * @brief The unit's own: MCUboot's slot trailer and the `watchdog0` alias (none: the machine still runs,
     *        a failure then resets nothing), timing from the EERIE_LEAP_DFU_* options.
     */
    DfuController();

    /**
     * @brief For tests: the bootloader, the watchdog and the timing are injected.
     *
     * @param boot_control Outlives the controller.
     * @param watchdog Outlives the controller; null for none.
     */
    DfuController(IBootControl& boot_control, IWatchdog* watchdog, ImageConfirmationConfig config);

    ~DfuController() override;

    DfuController(const DfuController&) = delete;
    DfuController& operator=(const DfuController&) = delete;
    DfuController(DfuController&&) = delete;
    DfuController& operator=(DfuController&&) = delete;

    /**
     * @brief Reads the confirmation state and the reset cause, and arms the watchdog when unconfirmed.
     *
     * Never waits: a confirmed image only reads the slot trailer. Registers the upload guard and the
     * `dfu` group, probes the MCUmgr work queue and starts the periodic tick while the image is unconfirmed.
     *
     * @return 0 on success, otherwise a negative errno. The confirmation still runs when the watchdog
     *         could not be armed; a failure to register the guard or the group is reported.
     */
    [[nodiscard]] int Initialize();

    /**
     * @brief Adds a probe that proves the work queue @p work_q still runs, for as long as the image is unconfirmed.
     *
     * The controller owns the probe. Does nothing for a confirmed image, which needs no probes.
     *
     * @param name Names the probe in the log; must outlive the controller (a literal).
     * @param work_q An initialized work queue that outlives the controller.
     */
    void RegisterLivenessProbe(const char* name, k_work_q* work_q);

    /**
     * @brief The unit can be managed again: the image confirms itself after the delay, if it keeps being healthy.
     *
     * Ignored by a build with EERIE_LEAP_DFU_BENCH_FAULT_NO_REPORT while the image is unconfirmed.
     */
    void ReportBootComplete();

    [[nodiscard]] DfuState GetState() const;

    /** @brief For `dfu STATUS`: any thread, including the MCUmgr work queue; never blocks. */
    [[nodiscard]] DfuStatus GetStatus() const override;
};

} // namespace eerie_leap::controllers
