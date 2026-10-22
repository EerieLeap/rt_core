#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include <zephyr/kernel.h>

#include "dfu_status.h"
#include "i_boot_control.h"
#include "i_probe.h"
#include "i_watchdog.h"

namespace eerie_leap::subsys::dfu {

/** @brief The timing and policy of ImageConfirmation; DfuController fills it from the Kconfig options. */
struct ImageConfirmationConfig {
    /// False leaves the confirmation to a client (bench): the image never confirms itself.
    bool self_confirm = true;
    /// Boot to ReportBootComplete(), for an unconfirmed image.
    uint32_t boot_deadline_ms = 60000;
    /// ReportBootComplete() to the confirmation, with the probes passing.
    uint32_t confirm_delay_ms = 20000;
    /// The hardware watchdog's timeout while the image is unconfirmed.
    uint32_t watchdog_timeout_ms = 8000;
};

/**
 * @brief Decides when a new (unconfirmed) image proves healthy and confirms it, or stops feeding the watchdog.
 *
 * The D6 state machine of FIRMWARE_UPDATE_PLAN, free of Zephyr drivers: the bootloader, the watchdog, the
 * probes and the clock are injected.
 *
 * - Initialize(): a confirmed image stays CONFIRMED and nothing is armed (one IsConfirmed() call, no
 *   waiting). An unconfirmed one (a test boot) installs and starts the watchdog, sets the boot deadline and
 *   is VERIFYING.
 * - ReportBootComplete(): VERIFYING becomes CONFIRMING; Confirm() follows once the delay has passed, on a tick
 *   where every probe has run since it was posted: the image is never confirmed while a probe is outstanding,
 *   so a queue that is stuck when the delay expires delays the confirmation to the next tick, and the
 *   probe's time limit (below) then decides.
 * - Tick(), every GetTickIntervalMs(): feeds the watchdog. In VERIFYING the probes are not judged: a unit
 *   that is still booting runs its init code at a higher priority than the work queues the probes watch,
 *   which starve for as long as it takes, and the boot deadline already catches a boot that hangs. In
 *   CONFIRMING a probe that has waited half the watchdog timeout or more without running makes the state FAILED, and
 *   so does a missed boot deadline. FAILED never feeds again, so the watchdog resets the unit and the
 *   bootloader reverts. An image that a client confirmed meanwhile becomes CONFIRMED and the watchdog is
 *   disabled, from FAILED too, as long as the watchdog has not fired.
 *
 * Thread safety: Tick() runs on one work queue, Initialize(), AddProbe() and ReportBootComplete() on the
 * main thread (before or while Tick() runs), and the getters on any thread, the MCUmgr work queue among
 * them. The state is atomic; the probe list is behind a mutex that is never held across a flash write or a
 * watchdog call.
 */
class ImageConfirmation final {
public:
    /// Milliseconds on a monotonic clock, like k_uptime_get.
    using Clock = std::function<int64_t()>;

private:
    struct ProbeEntry {
        IProbe* probe = nullptr;
        /// False until the first tick in CONFIRMING posts the probe, which therefore gets its time to run.
        bool armed = false;
        /// When the probe was last posted. Milliseconds, wrapping.
        uint32_t armed_at_ms = 0;
    };

    IBootControl& boot_control_;
    IWatchdog* watchdog_;
    Clock clock_;
    ImageConfirmationConfig config_;

    k_mutex probes_mutex_{};
    std::vector<ProbeEntry> probes_;

    std::atomic<DfuState> state_{ DfuState::CONFIRMED };
    /// The watchdog is running and has not been disabled yet.
    std::atomic<bool> is_watchdog_armed_{ false };
    /// Written by Initialize() only, before anything else uses the machine. Milliseconds, wrapping.
    uint32_t deadline_ms_ = 0;
    /// Written by ReportBootComplete() before it publishes CONFIRMING. Milliseconds, wrapping.
    std::atomic<uint32_t> confirm_at_ms_{ 0 };

    [[nodiscard]] uint32_t NowMs() const;
    /// True when @p now has reached @p at, correct across the wrap of the 32-bit millisecond count.
    [[nodiscard]] static bool IsReached(uint32_t now, uint32_t at);

    [[nodiscard]] int ArmWatchdog();
    void DisableWatchdog();
    /// The image is permanent: CONFIRMED, and the watchdog off.
    void StandDown();
    /**
     * @brief Judges the probes that were posted earlier and posts the ones that ran.
     *
     * @param all_ran Set to whether every probe ran since it was posted, none was posted just now and none is
     *                still waiting. The image is confirmed only on a tick where it is true.
     * @return False when a probe has waited half the watchdog timeout or more without running: the unit is
     *         not healthy.
     */
    [[nodiscard]] bool CheckAndRearmProbes(uint32_t now, bool& all_ran);

public:
    /**
     * @param boot_control Outlives the machine.
     * @param watchdog Outlives the machine; null on a board without a watchdog (the machine still tracks
     *                 the state and confirms, but a FAILED image is not reset).
     * @param clock Milliseconds since boot.
     * @param config Timing and policy.
     */
    ImageConfirmation(IBootControl& boot_control, IWatchdog* watchdog, Clock clock, ImageConfirmationConfig config);

    ImageConfirmation(const ImageConfirmation&) = delete;
    ImageConfirmation& operator=(const ImageConfirmation&) = delete;
    ImageConfirmation(ImageConfirmation&&) = delete;
    ImageConfirmation& operator=(ImageConfirmation&&) = delete;
    ~ImageConfirmation() = default;

    /**
     * @brief Reads the confirmation once and arms the watchdog and the boot deadline when unconfirmed.
     *
     * Never waits. When the watchdog cannot be armed, the machine still runs (it can confirm, and the
     * deadline and the probes still decide FAILED) but no reset follows a failure.
     *
     * @return 0, or the negative errno of the watchdog call that failed.
     */
    [[nodiscard]] int Initialize();

    /**
     * @brief Registers a probe; it outlives the machine. Safe while Tick() runs.
     *
     * The probe is posted by the first tick in CONFIRMING and judged from the next one on, so it has its
     * time to run.
     */
    void AddProbe(IProbe& probe);

    /**
     * @brief The unit can be managed again: VERIFYING becomes CONFIRMING and the delay starts.
     *
     * Reported past the boot deadline, the state becomes FAILED instead. A call in any other state does
     * nothing.
     */
    void ReportBootComplete();

    /** @brief One watchdog interval of work; see the class description. Never blocks beyond a flash write. */
    void Tick();

    /** @brief The period at which the owner calls Tick(): a quarter of the watchdog timeout, at least 1 ms. */
    [[nodiscard]] uint32_t GetTickIntervalMs() const;

    /**
     * @brief True while a Tick() has work to do: an armed state, or a watchdog still to be disabled.
     *
     * A FAILED image counts while the watchdog runs: a client's confirm before the reset stands it down.
     */
    [[nodiscard]] bool NeedsTick() const;

    [[nodiscard]] DfuState GetState() const;

    /** @brief True when the state is CONFIRMED. */
    [[nodiscard]] bool IsConfirmed() const;

    /**
     * @brief Time left until the image confirms itself; only in CONFIRMING with self-confirmation on.
     *
     * Reaches 0 when the delay has passed and the next tick will write the confirmation.
     */
    [[nodiscard]] std::optional<uint32_t> GetConfirmInMs() const;
};

} // namespace eerie_leap::subsys::dfu
