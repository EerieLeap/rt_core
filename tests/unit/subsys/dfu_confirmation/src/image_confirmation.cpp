#include <cerrno>
#include <cstdint>
#include <memory>

#include <zephyr/ztest.h>

#include "subsys/dfu/image_confirmation.h"

#include "dfu_fakes.h"

using eerie_leap::subsys::dfu::DfuState;
using eerie_leap::subsys::dfu::ImageConfirmation;
using eerie_leap::subsys::dfu::ImageConfirmationConfig;

using dfu_fakes::FakeBootControl;
using dfu_fakes::FakeProbe;
using dfu_fakes::FakeWatchdog;

namespace {

constexpr int64_t TICK_MS = 2000; // The default watchdog timeout (8000 ms) over four.
constexpr int64_t BOOT_DEADLINE_MS = 60000;
constexpr int64_t CONFIRM_DELAY_MS = 20000;

/** The machine under test with fakes for everything around it and a clock the test moves by hand. */
struct Harness {
    FakeBootControl boot;
    FakeWatchdog watchdog;
    FakeProbe probe_a{ "a" };
    FakeProbe probe_b{ "b" };
    int64_t now_ms = 5000;
    ImageConfirmationConfig config{
        .self_confirm = true,
        .boot_deadline_ms = BOOT_DEADLINE_MS,
        .confirm_delay_ms = CONFIRM_DELAY_MS,
        .watchdog_timeout_ms = 8000,
    };
    std::unique_ptr<ImageConfirmation> machine;

    Harness() = default;
    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;
    Harness(Harness&&) = delete;
    Harness& operator=(Harness&&) = delete;
    ~Harness() = default;

    ImageConfirmation& Create(bool with_watchdog = true) {
        machine = std::make_unique<ImageConfirmation>(
            boot, with_watchdog ? &watchdog : nullptr,
            [this] {
                return now_ms;
            },
            config
        );

        return *machine;
    }

    /** Lets time pass, the armed probes run (the stuck ones do not) and the machine tick. */
    void Step(int64_t ms = TICK_MS) {
        now_ms += ms;
        probe_a.Run();
        probe_b.Run();
        machine->Tick();
    }

    void StepTicks(int count) {
        for(int i = 0; i < count; ++i)
            Step();
    }

    /** An unconfirmed image with both probes registered and one healthy tick behind it. */
    ImageConfirmation& StartVerifying() {
        ImageConfirmation& m = Create();
        m.AddProbe(probe_a);
        m.AddProbe(probe_b);
        zassert_equal(m.Initialize(), 0);
        Step();

        return m;
    }

    /** StartVerifying() and the boot reported complete right after a healthy tick. */
    ImageConfirmation& StartConfirming() {
        ImageConfirmation& m = StartVerifying();
        m.ReportBootComplete();

        return m;
    }
};

void AssertState(const ImageConfirmation& machine, DfuState expected) {
    zassert_equal(static_cast<uint32_t>(machine.GetState()), static_cast<uint32_t>(expected));
}

} // namespace

ZTEST_SUITE(image_confirmation, NULL, NULL, NULL, NULL, NULL);

ZTEST(image_confirmation, test_confirmed_image_arms_nothing) {
    Harness h;
    h.boot.confirmed = true;
    ImageConfirmation& m = h.Create();
    m.AddProbe(h.probe_a);

    zassert_equal(m.Initialize(), 0);
    m.ReportBootComplete();
    h.StepTicks(3);

    AssertState(m, DfuState::CONFIRMED);
    zassert_true(m.IsConfirmed());
    zassert_false(m.NeedsTick());
    zassert_false(m.GetConfirmInMs().has_value());
    zassert_equal(h.boot.is_confirmed_calls, 1, "Initialize reads the confirmation once, and nothing else does");
    zassert_equal(h.boot.confirm_calls, 0);
    zassert_equal(h.watchdog.install_calls, 0);
    zassert_equal(h.watchdog.start_calls, 0);
    zassert_equal(h.watchdog.feed_calls, 0);
    zassert_equal(h.watchdog.disable_calls, 0);
    zassert_equal(h.probe_a.arm_calls, 0);
}

ZTEST(image_confirmation, test_unconfirmed_image_arms_the_watchdog_and_waits_for_boot_complete) {
    Harness h;
    ImageConfirmation& m = h.Create();

    zassert_equal(m.Initialize(), 0);

    AssertState(m, DfuState::VERIFYING);
    zassert_false(m.IsConfirmed());
    zassert_true(m.NeedsTick());
    zassert_false(m.GetConfirmInMs().has_value());
    zassert_equal(h.watchdog.install_calls, 1);
    zassert_equal(h.watchdog.installed_timeout_ms, 8000U);
    zassert_equal(h.watchdog.start_calls, 1);
    zassert_true(h.watchdog.is_running);
    zassert_equal(h.watchdog.feed_calls, 0);
    zassert_equal(h.boot.confirm_calls, 0);
    zassert_equal(m.GetTickIntervalMs(), static_cast<uint32_t>(TICK_MS));
}

ZTEST(image_confirmation, test_unconfirmed_image_confirms_after_boot_complete_plus_delay_then_disables_the_watchdog) {
    Harness h;
    ImageConfirmation& m = h.StartVerifying();
    h.Step();
    zassert_equal(h.watchdog.feed_calls, 2);

    m.ReportBootComplete();
    AssertState(m, DfuState::CONFIRMING);
    zassert_true(m.NeedsTick());

    // 18 s after boot complete: still healthy, still fed, not yet confirmed.
    h.StepTicks(9);
    AssertState(m, DfuState::CONFIRMING);
    zassert_equal(h.boot.confirm_calls, 0);
    zassert_equal(h.watchdog.feed_calls, 2 + 9);
    zassert_true(h.watchdog.is_running);

    // 20 s: the delay has passed.
    h.Step();
    AssertState(m, DfuState::CONFIRMED);
    zassert_true(m.IsConfirmed());
    zassert_equal(h.boot.confirm_calls, 1);
    zassert_equal(h.watchdog.disable_calls, 1);
    zassert_false(h.watchdog.is_running);
    zassert_false(m.NeedsTick());
    zassert_false(m.GetConfirmInMs().has_value());

    // Nothing is left to do.
    const int feeds = h.watchdog.feed_calls;
    h.StepTicks(3);
    zassert_equal(h.watchdog.feed_calls, feeds);
    zassert_equal(h.watchdog.disable_calls, 1);
    zassert_equal(h.boot.confirm_calls, 1);
}

ZTEST(image_confirmation, test_probes_are_not_posted_while_verifying) {
    Harness h;
    ImageConfirmation& m = h.StartVerifying();

    h.StepTicks(4);

    AssertState(m, DfuState::VERIFYING);
    zassert_equal(h.probe_a.arm_calls, 0);
    zassert_equal(h.probe_b.arm_calls, 0);
    zassert_equal(h.watchdog.feed_calls, 5);
}

ZTEST(image_confirmation, test_probes_are_posted_every_tick_once_the_boot_is_complete) {
    Harness h;
    ImageConfirmation& m = h.StartConfirming();
    zassert_equal(h.probe_a.arm_calls, 0, "Boot complete alone posts nothing; the next tick does");

    h.Step();
    zassert_equal(h.probe_a.arm_calls, 1);
    zassert_equal(h.probe_b.arm_calls, 1);

    h.StepTicks(4);

    AssertState(m, DfuState::CONFIRMING);
    zassert_equal(h.probe_a.arm_calls, 5);
    zassert_equal(h.probe_b.arm_calls, 5);
    zassert_equal(h.watchdog.feed_calls, 6);
}

ZTEST(image_confirmation, test_stuck_probe_does_not_fail_a_verifying_image) {
    Harness h;
    ImageConfirmation& m = h.StartVerifying();

    // The init code of a booting unit can keep the work queues from running for a long time.
    h.probe_a.is_stuck = true;
    h.probe_b.is_stuck = true;
    h.StepTicks(20);

    AssertState(m, DfuState::VERIFYING);
    zassert_equal(h.watchdog.feed_calls, 21, "The boot deadline decides, not the probes");
    zassert_true(m.NeedsTick());
}

ZTEST(image_confirmation, test_probes_starved_during_the_boot_get_their_time_after_boot_complete) {
    Harness h;
    ImageConfirmation& m = h.StartVerifying();
    h.probe_a.is_stuck = true;
    h.StepTicks(10);

    // The unit is up and the queues run again.
    h.probe_a.is_stuck = false;
    m.ReportBootComplete();
    h.StepTicks(3);

    AssertState(m, DfuState::CONFIRMING);
    zassert_equal(h.watchdog.feed_calls, 1 + 10 + 3);
}

ZTEST(image_confirmation, test_stuck_probe_fails_a_confirming_image) {
    Harness h;
    ImageConfirmation& m = h.StartConfirming();
    h.StepTicks(4);
    AssertState(m, DfuState::CONFIRMING);

    // Waiting for less than half the watchdog timeout (4 s) is allowed, and the unit is fed meanwhile.
    h.probe_a.is_stuck = true;
    h.Step();
    AssertState(m, DfuState::CONFIRMING);
    const int feeds = h.watchdog.feed_calls;

    // 4 s: the probe has waited half the timeout.
    h.Step();
    AssertState(m, DfuState::FAILED);
    zassert_equal(h.watchdog.feed_calls, feeds, "The tick that finds a stuck probe does not feed");
    zassert_true(h.watchdog.is_running, "The watchdog is left to reset the unit");

    h.probe_a.is_stuck = false;
    h.StepTicks(20);
    AssertState(m, DfuState::FAILED);
    zassert_equal(h.watchdog.feed_calls, feeds);
    zassert_equal(h.boot.confirm_calls, 0, "An unhealthy image is never confirmed");
    zassert_equal(h.watchdog.disable_calls, 0);
    zassert_false(m.GetConfirmInMs().has_value());
}

ZTEST(image_confirmation, test_the_image_is_not_confirmed_while_a_probe_is_outstanding) {
    Harness h;
    ImageConfirmation& m = h.StartConfirming();

    // 18 s after boot complete, healthy: every probe was posted again by this tick.
    h.StepTicks(9);
    AssertState(m, DfuState::CONFIRMING);
    zassert_equal(h.boot.confirm_calls, 0);

    // The queue gets stuck just before the delay expires (20 s): the probe posted at 18 s is outstanding.
    h.probe_a.is_stuck = true;
    h.Step();
    AssertState(m, DfuState::CONFIRMING);
    zassert_equal(h.boot.confirm_calls, 0, "The delay has passed, but a probe has not run since it was posted");
    zassert_true(h.watchdog.is_running);
    zassert_equal(m.GetConfirmInMs().value_or(1U), 0U);

    // Half the watchdog timeout (4 s) after it was posted, the probe fails the image instead.
    h.Step();
    AssertState(m, DfuState::FAILED);
    zassert_equal(h.boot.confirm_calls, 0);

    h.probe_a.is_stuck = false;
    h.StepTicks(5);
    AssertState(m, DfuState::FAILED);
    zassert_equal(h.boot.confirm_calls, 0);
}

ZTEST(image_confirmation, test_the_confirmation_follows_a_probe_that_runs_late) {
    Harness h;
    ImageConfirmation& m = h.StartConfirming();
    h.StepTicks(9);

    h.probe_a.is_stuck = true;
    h.Step();
    AssertState(m, DfuState::CONFIRMING);
    zassert_equal(h.boot.confirm_calls, 0);

    // The queue gets to the probe before its time is up: the next tick confirms.
    h.probe_a.is_stuck = false;
    h.Step();
    AssertState(m, DfuState::CONFIRMED);
    zassert_equal(h.boot.confirm_calls, 1);
}

ZTEST(image_confirmation, test_a_probe_posted_this_tick_has_not_run_yet) {
    Harness h;
    h.config.confirm_delay_ms = 0;
    ImageConfirmation& m = h.StartConfirming();

    // The delay is over at once, but the first tick after boot complete only posts the probes.
    h.Step();
    AssertState(m, DfuState::CONFIRMING);
    zassert_equal(h.boot.confirm_calls, 0);

    h.Step();
    AssertState(m, DfuState::CONFIRMED);
    zassert_equal(h.boot.confirm_calls, 1);
}

ZTEST(image_confirmation, test_a_probe_that_runs_late_keeps_the_image_healthy) {
    Harness h;
    ImageConfirmation& m = h.StartConfirming();
    h.StepTicks(4);

    // The queue is busy for one tick, then gets to the probe: it ran after 4 s, which is late but ran.
    h.probe_b.is_stuck = true;
    h.Step();
    h.probe_b.is_stuck = false;
    h.Step();
    AssertState(m, DfuState::CONFIRMING);
    const int feeds = h.watchdog.feed_calls;

    h.StepTicks(3);
    AssertState(m, DfuState::CONFIRMING);
    zassert_equal(h.watchdog.feed_calls, feeds + 3);
}

ZTEST(image_confirmation, test_a_new_probe_gets_its_time_to_run) {
    Harness h;
    ImageConfirmation& m = h.StartConfirming();
    h.StepTicks(2);

    FakeProbe late("late");
    late.is_stuck = true;
    m.AddProbe(late);
    zassert_equal(late.arm_calls, 0, "AddProbe does not post the probe");

    // The first tick after AddProbe posts it, and does not judge it yet.
    h.Step();
    AssertState(m, DfuState::CONFIRMING);
    zassert_equal(late.arm_calls, 1);

    // Within its time.
    h.Step();
    AssertState(m, DfuState::CONFIRMING);

    // Half the watchdog timeout after it was posted, it did not run.
    h.Step();
    AssertState(m, DfuState::FAILED);
}

ZTEST(image_confirmation, test_client_confirm_stands_down_a_failed_image) {
    Harness h;
    ImageConfirmation& m = h.StartConfirming();
    h.StepTicks(2);
    h.probe_a.is_stuck = true;
    h.StepTicks(2);
    AssertState(m, DfuState::FAILED);
    zassert_true(m.NeedsTick(), "The watchdog still runs, so a client's confirm can still save the image");

    h.boot.confirmed = true;
    h.Step();

    AssertState(m, DfuState::CONFIRMED);
    zassert_equal(h.boot.confirm_calls, 0);
    zassert_equal(h.watchdog.disable_calls, 1);
    zassert_false(h.watchdog.is_running);
    zassert_false(m.NeedsTick());
}

ZTEST(image_confirmation, test_client_confirm_stands_down_an_image_that_missed_its_deadline) {
    Harness h;
    ImageConfirmation& m = h.StartVerifying();
    h.StepTicks(30);
    AssertState(m, DfuState::FAILED);

    h.boot.confirmed = true;
    h.Step();

    AssertState(m, DfuState::CONFIRMED);
    zassert_false(h.watchdog.is_running);
}

ZTEST(image_confirmation, test_a_failed_image_without_a_watchdog_needs_no_tick) {
    Harness h;
    ImageConfirmation& m = h.Create(false);
    zassert_equal(m.Initialize(), 0);

    h.StepTicks(30);

    AssertState(m, DfuState::FAILED);
    zassert_false(m.NeedsTick());
}

ZTEST(image_confirmation, test_missed_boot_deadline_stops_feeding) {
    Harness h;
    ImageConfirmation& m = h.StartVerifying();

    // 58 s after Initialize: still inside the deadline.
    h.StepTicks(28);
    AssertState(m, DfuState::VERIFYING);
    zassert_equal(h.watchdog.feed_calls, 29);

    // 60 s: the deadline.
    h.Step();
    AssertState(m, DfuState::FAILED);
    zassert_equal(h.watchdog.feed_calls, 29);

    h.StepTicks(5);
    AssertState(m, DfuState::FAILED);
    zassert_equal(h.watchdog.feed_calls, 29);
    zassert_equal(h.boot.confirm_calls, 0);
}

ZTEST(image_confirmation, test_boot_complete_after_the_deadline_fails_the_image) {
    Harness h;
    ImageConfirmation& m = h.StartVerifying();

    h.now_ms += BOOT_DEADLINE_MS - TICK_MS;
    m.ReportBootComplete();

    AssertState(m, DfuState::FAILED);
    zassert_false(m.GetConfirmInMs().has_value());

    h.StepTicks(3);
    AssertState(m, DfuState::FAILED);
    zassert_equal(h.watchdog.feed_calls, 1);
    zassert_equal(h.boot.confirm_calls, 0);
}

ZTEST(image_confirmation, test_boot_complete_just_inside_the_deadline_is_accepted) {
    Harness h;
    ImageConfirmation& m = h.StartVerifying();

    h.now_ms += BOOT_DEADLINE_MS - TICK_MS - 1;
    m.ReportBootComplete();

    AssertState(m, DfuState::CONFIRMING);
}

ZTEST(image_confirmation, test_client_confirm_stands_down_a_verifying_image) {
    Harness h;
    ImageConfirmation& m = h.StartVerifying();

    h.boot.confirmed = true;
    h.Step();

    AssertState(m, DfuState::CONFIRMED);
    zassert_equal(h.boot.confirm_calls, 0, "The machine does not write a confirmation that is already there");
    zassert_equal(h.watchdog.disable_calls, 1);
    zassert_false(h.watchdog.is_running);
    zassert_false(m.NeedsTick());

    const int feeds = h.watchdog.feed_calls;
    h.StepTicks(3);
    zassert_equal(h.watchdog.feed_calls, feeds);
    zassert_equal(h.watchdog.disable_calls, 1);
}

ZTEST(image_confirmation, test_client_confirm_stands_down_a_confirming_image) {
    Harness h;
    ImageConfirmation& m = h.StartConfirming();
    h.StepTicks(3);

    h.boot.confirmed = true;
    h.Step();

    AssertState(m, DfuState::CONFIRMED);
    zassert_equal(h.boot.confirm_calls, 0);
    zassert_equal(h.watchdog.disable_calls, 1);
    zassert_false(m.GetConfirmInMs().has_value());

    h.StepTicks(15);
    zassert_equal(h.boot.confirm_calls, 0);
    zassert_equal(h.watchdog.disable_calls, 1);
}

ZTEST(image_confirmation, test_failed_confirm_write_is_retried) {
    Harness h;
    ImageConfirmation& m = h.StartConfirming();
    h.boot.confirm_result = -EIO;

    h.StepTicks(10);

    AssertState(m, DfuState::CONFIRMING);
    zassert_equal(h.boot.confirm_calls, 1);
    zassert_true(h.watchdog.is_running);
    zassert_equal(h.watchdog.disable_calls, 0);
    zassert_true(m.NeedsTick());
    zassert_equal(m.GetConfirmInMs().value_or(1U), 0U);

    // Still fed and retried on every tick.
    const int feeds = h.watchdog.feed_calls;
    h.Step();
    zassert_equal(h.boot.confirm_calls, 2);
    zassert_equal(h.watchdog.feed_calls, feeds + 1);
    AssertState(m, DfuState::CONFIRMING);

    h.boot.confirm_result = 0;
    h.Step();
    zassert_equal(h.boot.confirm_calls, 3);
    AssertState(m, DfuState::CONFIRMED);
    zassert_equal(h.watchdog.disable_calls, 1);
}

ZTEST(image_confirmation, test_without_self_confirm_the_image_never_confirms_but_keeps_feeding) {
    Harness h;
    h.config.self_confirm = false;
    ImageConfirmation& m = h.StartConfirming();
    const int feeds = h.watchdog.feed_calls;

    h.StepTicks(30);

    AssertState(m, DfuState::CONFIRMING);
    zassert_equal(h.boot.confirm_calls, 0);
    zassert_equal(h.watchdog.feed_calls, feeds + 30);
    zassert_true(h.watchdog.is_running);
    zassert_false(m.GetConfirmInMs().has_value(), "There is no self-confirmation to count down to");

    // A client's confirm still ends it.
    h.boot.confirmed = true;
    h.Step();
    AssertState(m, DfuState::CONFIRMED);
    zassert_equal(h.watchdog.disable_calls, 1);
}

ZTEST(image_confirmation, test_confirm_in_ms_counts_down) {
    Harness h;
    ImageConfirmation& m = h.StartVerifying();
    zassert_false(m.GetConfirmInMs().has_value(), "Not before boot complete");

    m.ReportBootComplete();
    zassert_equal(m.GetConfirmInMs().value_or(0U), 20000U);

    h.now_ms += 2500;
    zassert_equal(m.GetConfirmInMs().value_or(0U), 17500U);

    h.now_ms += 17500;
    zassert_equal(m.GetConfirmInMs().value_or(1U), 0U);

    h.now_ms += 5000;
    zassert_equal(m.GetConfirmInMs().value_or(1U), 0U, "It stops at 0");
}

ZTEST(image_confirmation, test_repeated_boot_complete_does_not_restart_the_delay) {
    Harness h;
    ImageConfirmation& m = h.StartConfirming();

    h.now_ms += 5000;
    m.ReportBootComplete();

    zassert_equal(m.GetConfirmInMs().value_or(0U), 15000U);
    AssertState(m, DfuState::CONFIRMING);
}

ZTEST(image_confirmation, test_a_board_without_a_watchdog_still_confirms) {
    Harness h;
    ImageConfirmation& m = h.Create(false);
    m.AddProbe(h.probe_a);

    zassert_equal(m.Initialize(), 0);
    AssertState(m, DfuState::VERIFYING);
    h.Step();
    m.ReportBootComplete();
    h.StepTicks(10);

    AssertState(m, DfuState::CONFIRMED);
    zassert_equal(h.boot.confirm_calls, 1);
    zassert_equal(h.watchdog.install_calls, 0);
    zassert_equal(h.watchdog.feed_calls, 0);
    zassert_equal(h.watchdog.disable_calls, 0);
}

ZTEST(image_confirmation, test_watchdog_install_failure_is_reported_and_the_machine_still_runs) {
    Harness h;
    h.watchdog.install_result = -EIO;
    ImageConfirmation& m = h.Create();

    zassert_equal(m.Initialize(), -EIO);
    AssertState(m, DfuState::VERIFYING);
    zassert_equal(h.watchdog.start_calls, 0);

    m.ReportBootComplete();
    h.StepTicks(10);

    AssertState(m, DfuState::CONFIRMED);
    zassert_equal(h.watchdog.feed_calls, 0);
    zassert_equal(h.watchdog.disable_calls, 0, "A watchdog that never ran is not disabled");
}

ZTEST(image_confirmation, test_watchdog_start_failure_is_reported_and_the_machine_still_runs) {
    Harness h;
    h.watchdog.start_result = -EIO;
    ImageConfirmation& m = h.Create();

    zassert_equal(m.Initialize(), -EIO);
    AssertState(m, DfuState::VERIFYING);

    m.ReportBootComplete();
    h.StepTicks(10);

    AssertState(m, DfuState::CONFIRMED);
    zassert_equal(h.watchdog.feed_calls, 0);
    zassert_equal(h.watchdog.disable_calls, 0);
}

ZTEST(image_confirmation, test_failed_watchdog_disable_is_retried_while_it_is_fed) {
    Harness h;
    ImageConfirmation& m = h.StartConfirming();
    h.boot.confirmed = true;
    h.watchdog.disable_result = -EIO;

    h.Step();

    AssertState(m, DfuState::CONFIRMED);
    zassert_equal(h.watchdog.disable_calls, 1);
    zassert_true(h.watchdog.is_running);
    zassert_true(m.NeedsTick(), "The watchdog must not be left to reset a confirmed image");

    const int feeds = h.watchdog.feed_calls;
    h.Step();
    zassert_equal(h.watchdog.disable_calls, 2);
    zassert_true(h.watchdog.feed_calls > feeds);

    h.watchdog.disable_result = 0;
    h.Step();
    zassert_equal(h.watchdog.disable_calls, 3);
    zassert_false(h.watchdog.is_running);
    zassert_false(m.NeedsTick());
}

ZTEST(image_confirmation, test_tick_interval_is_a_quarter_of_the_watchdog_timeout) {
    Harness h;

    h.config.watchdog_timeout_ms = 8000;
    zassert_equal(h.Create().GetTickIntervalMs(), 2000U);

    h.config.watchdog_timeout_ms = 3;
    zassert_equal(h.Create().GetTickIntervalMs(), 1U);

    h.config.watchdog_timeout_ms = 0;
    zassert_equal(h.Create().GetTickIntervalMs(), 1U);
}

ZTEST(image_confirmation, test_a_clock_that_wraps_in_the_middle_of_the_flow_is_handled) {
    Harness h;
    // Initialize and the boot deadline sit before the wrap, the confirmation after it.
    h.now_ms = 0x100000000LL - 12000;
    ImageConfirmation& m = h.StartVerifying();
    m.ReportBootComplete();
    zassert_equal(m.GetConfirmInMs().value_or(0U), 20000U);

    h.StepTicks(9);
    AssertState(m, DfuState::CONFIRMING);
    zassert_true(h.now_ms > 0x100000000LL);
    zassert_equal(m.GetConfirmInMs().value_or(0U), 2000U);

    h.Step();
    AssertState(m, DfuState::CONFIRMED);
    zassert_equal(h.boot.confirm_calls, 1);
}
