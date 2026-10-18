// LogWriterSupervisor's policy: requests run on the configuration work queue, the latest one wins
// and none waits for the writer; a failed session is stopped there, and a late failure event stops
// no later one; a stored logging configuration restarts a running writer only; the logger names the
// tool, the device and the sensors; and the supervisor goes away safely with a request pending, a
// run in progress, an event in flight or a store calling its handler, and after a failed Initialize().
//
// Code that runs on the configuration work queue or the bus thread never asserts: an assertion there
// aborts that thread. It records into a shared context that the test asserts on afterwards.
#include <atomic>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <string>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "domain/system_domain/models/system_configuration.h"
#include "subsys/threading/service_state.h"

#include "log_writer_probe.h"
#include "logging_test_support.h"

using namespace logging_test_support;

using eerie_leap::domain::logging_domain::control::ILoggingControl;
using eerie_leap::domain::system_domain::models::SystemConfiguration;
using eerie_leap::subsys::threading::ServiceState;

namespace {

// Bounds a call that must not wait for the writer: one that does waits a held tick of kHoldMs.
constexpr int64_t kNoWaitMs = ProbeLogger::kHoldMs / 2;

// A rig whose every tick offers a reading, and a probe per start: a session fails at the next tick
// once the current probe refuses a record.
struct FailingRig {
    std::vector<std::shared_ptr<Sensor>> sensors = MakeLoggedSensors();
    LogWriterRig rig = MakeLogWriterRig(sensors, false);
    std::shared_ptr<ProbeLoggerFactory> probes;
};

void UseProbes(FailingRig& failing) {
    failing.probes = ProbeLoggerFactory::Make(failing.rig.logger_factory);
    failing.rig.log_writer_service->SetLoggerFactory(failing.probes->AsFactory());
    PublishValue(failing.rig, *failing.sensors[0], 1, failing.rig.time_service->GetCurrentTime());
}

std::shared_ptr<Sensor> MakeVirtualSensor(std::string_view id) {
    auto sensor = std::make_shared<Sensor>(std::allocator_arg, Mrm::GetDefaultPmr(), id);
    sensor->configuration.type = SensorType::VIRTUAL_ANALOG;
    sensor->configuration.expression = "1 + 1";
    sensor->configuration.sampling_rate_ms = 20;

    return sensor;
}

std::shared_ptr<SensorsConfigurationManager> MakeSensorsManager(const LogWriterRig& rig) {
    return std::make_shared<SensorsConfigurationManager>(
        std::make_unique<CborConfigurationService<CborSensorsConfig>>(
            "sensors_config", rig.fs_service, rig.config_work_queue
        ),
        nullptr, 0, 0
    );
}

} // namespace

ZTEST_SUITE(log_writer_supervisor, NULL, NULL, ResetAfterTest, NULL, NULL);

// A start while running and a stop while stopped do nothing.
ZTEST(log_writer_supervisor, test_ARequestStartsAndStopsTheWriter) {
    auto rig = MakeLogWriterRig();
    const StatusRecorder recorder;
    auto& supervisor = AttachSupervisor(rig);

    zassert_false(supervisor.IsActive(), "nothing has been reported yet");

    supervisor.RequestStart(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(1), "the start must publish its status");
    zassert_true(
        WaitFor(
            [&] {
                return supervisor.IsActive();
            },
            1000
        ),
        "IsActive() must follow the event"
    );
    zassert_equal(CountLogFiles(*rig.fs_service), 1, "a start opens a file");

    supervisor.RequestStart(ILoggingControl::kAllUnits);
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    k_msleep(50);
    zassert_equal(CountLogFiles(*rig.fs_service), 1, "a start while running must not open another file");
    zassert_equal(recorder.GetEvents().size(), 1, "a start while running must publish nothing");

    supervisor.RequestStop(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(2), "the stop must publish its status");
    zassert_true(
        WaitFor(
            [&] {
                return !supervisor.IsActive();
            },
            1000
        ),
        "IsActive() must follow the event"
    );
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    zassert_true(rig.log_writer_service->IsStopped(), "the stop must leave the writer stopped");

    supervisor.RequestStop(ILoggingControl::kAllUnits);
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    k_msleep(50);
    zassert_equal(recorder.GetEvents().size(), 2, "a stop while stopped must publish nothing");

    const auto files = ReadLogFiles(*rig.fs_service);
    zassert_equal(files.size(), 1, "one file (found %zu)", files.size());
    AssertFinalizedFile(files[0].content, "the file");
}

// The CDMP LOGGING command another unit sent (LoggingCommandHandler) is a request, accepted whether or
// not it changes anything (L9).
ZTEST(log_writer_supervisor, test_ALoggingCommandFromAnotherUnitIsARequest) {
    auto rig = MakeLogWriterRig();
    const StatusRecorder recorder;
    auto& supervisor = AttachSupervisor(rig);

    zassert_true(supervisor.HandleLoggingCommand(true), "a start is accepted");
    zassert_true(recorder.WaitForEventCount(1), "and starts the writer");
    zassert_true(
        WaitFor(
            [&] {
                return supervisor.IsActive();
            },
            1000
        ),
        "IsActive() must follow the event"
    );
    zassert_equal(CountLogFiles(*rig.fs_service), 1, "in a file");

    zassert_true(supervisor.HandleLoggingCommand(false), "a stop is accepted");
    zassert_true(recorder.WaitForEventCount(2), "and stops it");
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    zassert_true(rig.log_writer_service->IsStopped());

    zassert_true(supervisor.HandleLoggingCommand(false), "a stop while stopped is accepted too");
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    k_msleep(50);
    zassert_equal(recorder.GetEvents().size(), 2, "and publishes nothing");
}

// Requests made before the task runs act once, as the last of them: start, stop, start is one start,
// and start, stop leaves the writer stopped. None of them waits.
ZTEST(log_writer_supervisor, test_TheLatestRequestWins) {
    auto rig = MakeLogWriterRig();
    const StatusRecorder recorder;
    auto& supervisor = AttachSupervisor(rig);

    zassert_true(Gate().Close(*rig.config_work_queue), "the configuration work queue could not be held");
    int64_t start_ms = k_uptime_get();
    supervisor.RequestStart(ILoggingControl::kAllUnits);
    supervisor.RequestStop(ILoggingControl::kAllUnits);
    supervisor.RequestStart(ILoggingControl::kAllUnits);
    int64_t elapsed_ms = k_uptime_get() - start_ms;
    zassert_true(elapsed_ms < kNoWaitMs, "the requests took %lld ms", static_cast<long long>(elapsed_ms));
    Gate().Open();

    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    zassert_true(recorder.WaitForEventCount(1), "the start must publish its status");
    k_msleep(50);
    zassert_true(rig.log_writer_service->IsRunning(), "the latest request, a start, must have won");
    zassert_equal(
        recorder.GetEvents().size(), 1, "the three requests must act once (%zu events)", recorder.GetEvents().size()
    );
    zassert_equal(CountLogFiles(*rig.fs_service), 1, "one start, one file");

    supervisor.RequestStop(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(2), "the stop must publish its status");
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");

    zassert_true(Gate().Close(*rig.config_work_queue), "the configuration work queue could not be held");
    start_ms = k_uptime_get();
    supervisor.RequestStart(ILoggingControl::kAllUnits);
    supervisor.RequestStop(ILoggingControl::kAllUnits);
    elapsed_ms = k_uptime_get() - start_ms;
    zassert_true(elapsed_ms < kNoWaitMs, "the requests took %lld ms", static_cast<long long>(elapsed_ms));
    Gate().Open();

    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    k_msleep(50);
    zassert_true(rig.log_writer_service->IsStopped(), "the latest request, a stop, must have won");
    zassert_equal(
        recorder.GetEvents().size(), 2, "the start that lost must publish nothing (%zu events)",
        recorder.GetEvents().size()
    );
    zassert_equal(CountLogFiles(*rig.fs_service), 1, "the start that lost must open no file");
}

// While the task waits inside the writer's Stop() for a held tick, another request and IsActive()
// return at once; the request runs once the stop is done.
ZTEST(log_writer_supervisor, test_ARequestNeverWaitsForTheWriter) {
    const auto sensors = MakeLoggedSensors();
    auto rig = MakeLogWriterRig(sensors);
    const StatusRecorder recorder;
    auto& supervisor = AttachSupervisor(rig);
    const auto probes = ProbeLoggerFactory::Make(rig.logger_factory);
    rig.log_writer_service->SetLoggerFactory(probes->AsFactory());

    supervisor.RequestStart(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(1), "the start must publish its status");

    const auto probe = probes->Current();
    probe->hold_next_reading = true;
    PublishValue(rig, *sensors[0], 7, rig.time_service->GetCurrentTime());
    zassert_equal(k_sem_take(&probe->held_reading_started, K_SECONDS(2)), 0, "a tick must log the published value");

    int64_t start_ms = k_uptime_get();
    supervisor.RequestStop(ILoggingControl::kAllUnits);
    int64_t elapsed_ms = k_uptime_get() - start_ms;
    zassert_true(
        elapsed_ms < kNoWaitMs, "RequestStop(ILoggingControl::kAllUnits) took %lld ms",
        static_cast<long long>(elapsed_ms)
    );

    zassert_true(
        WaitFor(
            [&] {
                return rig.log_writer_service->GetState() == ServiceState::STOPPING;
            },
            1000
        ),
        "the task must be inside the writer's Stop(), waiting for the held tick"
    );

    start_ms = k_uptime_get();
    supervisor.RequestStart(ILoggingControl::kAllUnits);
    const bool is_active = supervisor.IsActive();
    elapsed_ms = k_uptime_get() - start_ms;
    zassert_true(
        elapsed_ms < kNoWaitMs,
        "RequestStart(ILoggingControl::kAllUnits) and IsActive() took %lld ms while the writer stopped",
        static_cast<long long>(elapsed_ms)
    );
    zassert_true(is_active, "the stop has not been published yet");

    zassert_true(recorder.WaitForEventCount(3), "the stop and the queued start must each publish a status");
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    zassert_true(rig.log_writer_service->IsRunning(), "the start requested during the stop must run after it");
    zassert_true(probe->is_held_reading_done.load(), "the stop must have waited for the held tick");

    const auto events = recorder.GetEvents();
    zassert_true(events[0].is_active && !events[1].is_active && events[2].is_active, "start, stop, start");
    zassert_equal(CountLogFiles(*rig.fs_service), 2, "the second start opens a second file");

    supervisor.RequestStop(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(4), "the stop must publish its status");
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
}

// The failure event reschedules the task; the stop waits for the configuration work queue and runs
// there. A stored configuration does not start the stopped writer again.
ZTEST(log_writer_supervisor, test_AFailedSessionIsStoppedOnTheConfigurationWorkQueue) {
    FailingRig failing;
    auto& rig = failing.rig;
    const StatusRecorder recorder;
    auto& supervisor = AttachSupervisor(rig);
    UseProbes(failing);

    supervisor.RequestStart(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(1), "the start must publish its status");
    const auto probe = failing.probes->Current();

    zassert_true(Gate().Close(*rig.config_work_queue), "the configuration work queue could not be held");
    probe->refuse_next_reading = true;
    zassert_true(recorder.WaitForEventCount(2), "the failure must be published");
    zassert_true(recorder.GetEvents()[1].reason == LoggingStatusReason::WriteFailed, "a refused record is WriteFailed");
    zassert_true(
        WaitFor(
            [&] {
                return !supervisor.IsActive();
            },
            1000
        ),
        "IsActive() must follow the failure"
    );

    k_msleep(100);
    zassert_true(rig.log_writer_service->IsRunning(), "the stop must wait for the configuration work queue");
    zassert_true(rig.log_writer_service->HasFailed(), "the session has failed");
    zassert_is_null(probe->stop_thread.load(), "nothing may have finalized the file yet");

    Gate().Open();
    zassert_true(
        WaitFor(
            [&] {
                return rig.log_writer_service->IsStopped();
            },
            2000
        ),
        "the failed session must be stopped"
    );
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    zassert_equal(
        probe->stop_thread.load(), ConfigQueueThread(rig), "the stop must run on the configuration work queue"
    );

    zassert_true(recorder.WaitForEventCount(3), "the stop must publish its status");
    zassert_true(recorder.GetEvents()[2].reason == LoggingStatusReason::Stopped, "the stop publishes Stopped");
    zassert_false(supervisor.IsActive(), "a stopped session is not active");

    StoreLoggingConfiguration(rig, 50);
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    k_msleep(50);
    zassert_true(rig.log_writer_service->IsStopped(), "a stored configuration must not start a failed writer");
    zassert_equal(CountLogFiles(*rig.fs_service), 1, "no file may be opened");
    zassert_equal(recorder.GetEvents().size(), 3, "nothing may be published");
}

// A start that finds the session failed closes it and opens a new file.
ZTEST(log_writer_supervisor, test_AStartOfAFailedSessionStopsItAndStartsANewFile) {
    FailingRig failing;
    auto& rig = failing.rig;
    const StatusRecorder recorder;
    auto& supervisor = AttachSupervisor(rig);
    UseProbes(failing);

    supervisor.RequestStart(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(1), "the start must publish its status");

    zassert_true(Gate().Close(*rig.config_work_queue), "the configuration work queue could not be held");
    failing.probes->Current()->refuse_next_reading = true;
    zassert_true(recorder.WaitForEventCount(2), "the failure must be published");
    supervisor.RequestStart(ILoggingControl::kAllUnits);
    Gate().Open();

    zassert_true(recorder.WaitForEventCount(4), "the stop of the failed session and the new start must be published");
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    zassert_true(rig.log_writer_service->IsRunning(), "the start must run a new session");
    zassert_false(rig.log_writer_service->HasFailed(), "the new session has not failed");
    zassert_equal(CountLogFiles(*rig.fs_service), 2, "the failed file and the new one");

    const auto events = recorder.GetEvents();
    zassert_true(events[0].is_active, "the first start");
    zassert_true(events[1].reason == LoggingStatusReason::WriteFailed, "the failure");
    zassert_true(events[2].reason == LoggingStatusReason::Stopped, "the failed session is stopped first");
    zassert_true(events[3].is_active, "then the new session starts");

    supervisor.RequestStop(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(5), "the stop must publish its status");
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
}

// A failure event that reaches the supervisor after a start replaced the failed session stops
// nothing: the writer's own HasFailed() decides, not the event. The bus is held on the failure until
// the start has run, then on the stop of the failed session, so the check the late failure queues
// runs while that failure is the last status the supervisor saw. The bus thread is cooperative, so
// without the second hold it would deliver the stop and the new start before the check ran.
ZTEST(log_writer_supervisor, test_AFailureEventAfterANewStartStopsNothing) {
    FailingRig failing;
    auto& rig = failing.rig;
    const StatusRecorder recorder;
    // Before the supervisor, so its handler sees each held event only once the blocker lets it go.
    StatusBlocker blocker;
    auto& supervisor = AttachSupervisor(rig);
    UseProbes(failing);

    supervisor.RequestStart(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(1), "the start must publish its status");

    blocker.Arm();
    failing.probes->Current()->refuse_next_reading = true;
    zassert_true(blocker.WaitUntilBlocking(), "the failure event must reach the blocker");

    // The bus holds WriteFailed; the start closes the failed session and opens a new one first.
    supervisor.RequestStart(ILoggingControl::kAllUnits);
    zassert_true(
        WaitFor(
            [&] {
                return failing.probes->GetCount() == 2;
            },
            2000
        ),
        "the start must build a new session"
    );
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    zassert_true(rig.log_writer_service->IsRunning() && !rig.log_writer_service->HasFailed(), "the new session runs");

    // WriteFailed reaches the supervisor now and queues its failure check; the bus is held again on
    // the stop of the failed session, before the supervisor sees it.
    blocker.Arm();
    blocker.Unblock();
    zassert_true(blocker.WaitUntilBlocking(), "the stop of the failed session must reach the blocker");
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    k_msleep(50);

    zassert_false(supervisor.IsActive(), "the check must have run with the failure as the last status");
    zassert_true(rig.log_writer_service->IsRunning(), "a late failure event must not stop the new session");
    zassert_false(rig.log_writer_service->HasFailed(), "the new session has not failed");

    blocker.Unblock();
    zassert_true(recorder.WaitForEventCount(4), "the stop of the failed session and the new start must be published");
    zassert_true(
        WaitFor(
            [&] {
                return supervisor.IsActive();
            },
            1000
        ),
        "IsActive() must follow the new start"
    );
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    k_msleep(50);

    zassert_true(rig.log_writer_service->IsRunning(), "the new session must still run");
    zassert_equal(CountLogFiles(*rig.fs_service), 2, "the failed file and the new one");
    const auto events = recorder.GetEvents();
    zassert_equal(events.size(), 4, "start, failure, stop, start and nothing after (%zu events)", events.size());
    zassert_true(events[1].reason == LoggingStatusReason::WriteFailed, "the failure");
    zassert_true(events[2].reason == LoggingStatusReason::Stopped, "the stop of the failed session");
    zassert_true(events[3].is_active, "the new session is the last word");

    supervisor.RequestStop(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(5), "the stop must publish its status");
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
}

// The stop of the failure check fails once (the finalize throws). The start that follows in the same
// run stops the failed session itself before it opens a new file.
ZTEST(log_writer_supervisor, test_AStartStopsAFailedSessionTheFailureCheckCouldNotStop) {
    FailingRig failing;
    auto& rig = failing.rig;
    const StatusRecorder recorder;
    auto& supervisor = AttachSupervisor(rig);
    UseProbes(failing);

    supervisor.RequestStart(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(1), "the start must publish its status");
    const auto probe = failing.probes->Current();

    zassert_true(Gate().Close(*rig.config_work_queue), "the configuration work queue could not be held");
    probe->stop_throws = 1;
    probe->refuse_next_reading = true;
    zassert_true(recorder.WaitForEventCount(2), "the failure must be published");
    supervisor.RequestStart(ILoggingControl::kAllUnits);
    Gate().Open();

    zassert_true(recorder.WaitForEventCount(4), "the stop of the failed session and the new start must be published");
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    zassert_equal(probe->stop_throws.load(), 0, "the failure check must have tried the stop first");
    zassert_true(rig.log_writer_service->IsRunning(), "the start must run a new session");
    zassert_false(rig.log_writer_service->HasFailed(), "the new session has not failed");
    zassert_equal(failing.probes->GetCount(), 2, "the start must build a new session");
    zassert_equal(CountLogFiles(*rig.fs_service), 2, "the failed file and the new one");

    const auto events = recorder.GetEvents();
    zassert_true(events[2].reason == LoggingStatusReason::Stopped, "the failed session is stopped first");
    zassert_true(events[3].is_active, "then the new session starts");

    supervisor.RequestStop(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(5), "the stop must publish its status");
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
}

// A failed session that cannot be stopped (here every finalize throws) is not replaced: the writer
// would only report RUNNING, and a new session cannot start under it.
ZTEST(log_writer_supervisor, test_AStartThatCannotStopTheFailedSessionStartsNothing) {
    FailingRig failing;
    auto& rig = failing.rig;
    const StatusRecorder recorder;
    auto& supervisor = AttachSupervisor(rig);
    UseProbes(failing);

    supervisor.RequestStart(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(1), "the start must publish its status");
    const auto probe = failing.probes->Current();

    zassert_true(Gate().Close(*rig.config_work_queue), "the configuration work queue could not be held");
    probe->stop_throws = 2;
    probe->refuse_next_reading = true;
    zassert_true(recorder.WaitForEventCount(2), "the failure must be published");
    supervisor.RequestStart(ILoggingControl::kAllUnits);
    Gate().Open();

    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    k_msleep(50);
    zassert_equal(probe->stop_throws.load(), 0, "the failure check and the start must each have tried the stop");
    zassert_true(rig.log_writer_service->IsRunning(), "a stop that throws leaves the writer RUNNING");
    zassert_true(rig.log_writer_service->HasFailed(), "the failed session must still be the writer's");
    zassert_equal(failing.probes->GetCount(), 1, "no new session may have been built");
    zassert_equal(recorder.GetEvents().size(), 2, "nothing may be published (%zu events)", recorder.GetEvents().size());

    // Once the stop can finalize again, a request closes the session.
    supervisor.RequestStop(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(3), "the stop must publish its status");
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    zassert_true(rig.log_writer_service->IsStopped(), "the stop must close the failed session");
    zassert_equal(CountLogFiles(*rig.fs_service), 1, "one file");
}

// A start that fails leaves logging stopped: neither a stored configuration nor a sensors restart
// starts it later.
ZTEST(log_writer_supervisor, test_AFailedStartLeavesLoggingStopped) {
    auto rig = MakeLogWriterRig();
    const StatusRecorder recorder;
    auto& supervisor = AttachSupervisor(rig);
    const auto dependent = supervisor.GetDependentService();

    rig.log_writer_service->SetSizeLimitsForTesting(0, rig.fs_service->GetTotalSpace() + 1);
    supervisor.RequestStart(ILoggingControl::kAllUnits);
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    k_msleep(50);
    zassert_true(rig.log_writer_service->IsStopped(), "a start without the minimum free must fail");
    zassert_equal(CountLogFiles(*rig.fs_service), 0, "a failed start leaves no file");
    zassert_equal(recorder.GetEvents().size(), 0, "a failed start publishes nothing");
    zassert_false(supervisor.IsActive(), "nothing runs");

    rig.log_writer_service->SetSizeLimitsForTesting(0, 0);
    StoreLoggingConfiguration(rig, 50);
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");

    // What a sensors restart does with a dependent it stopped.
    const auto is_started = std::make_shared<std::atomic<bool>>(false);
    rig.config_work_queue->Run([dependent, is_started] {
        *is_started = dependent->Start();
    });
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    k_msleep(50);

    zassert_true(is_started->load(), "the dependent declines without an error");
    zassert_true(rig.log_writer_service->IsStopped(), "logging is no longer wanted after a failed start");
    zassert_equal(CountLogFiles(*rig.fs_service), 0, "no file may be opened");
    zassert_equal(recorder.GetEvents().size(), 0, "nothing may be published");
}

// A stored logging configuration restarts only a running writer.
ZTEST(log_writer_supervisor, test_AConfigurationApplyLeavesAStoppedWriterStopped) {
    auto rig = MakeLogWriterRig();
    const StatusRecorder recorder;
    auto& supervisor = AttachSupervisor(rig);

    StoreLoggingConfiguration(rig, 50);
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    k_msleep(50);
    zassert_true(rig.log_writer_service->IsStopped(), "a stored configuration must not start a writer that never ran");
    zassert_equal(CountLogFiles(*rig.fs_service), 0, "no file may be opened");

    supervisor.RequestStart(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(1), "the start must publish its status");
    supervisor.RequestStop(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(2), "the stop must publish its status");
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");

    StoreLoggingConfiguration(rig, 80);
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    k_msleep(50);
    zassert_true(rig.log_writer_service->IsStopped(), "a stored configuration must not start a stopped writer");
    zassert_equal(CountLogFiles(*rig.fs_service), 1, "no further file may be opened");
    zassert_equal(recorder.GetEvents().size(), 2, "nothing may be published");
}

// A configuration stored on the queue while the session has failed, before the supervisor stopped
// it: the run stops the failed session and restarts nothing.
ZTEST(log_writer_supervisor, test_AConfigurationApplyLeavesAFailedWriterStopped) {
    FailingRig failing;
    auto& rig = failing.rig;
    const StatusRecorder recorder;
    auto& supervisor = AttachSupervisor(rig);
    UseProbes(failing);

    supervisor.RequestStart(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(1), "the start must publish its status");

    struct Context {
        k_sem entered;
        std::atomic<bool> saw_failure = false;
        std::atomic<bool> is_stored = false;
    };
    const auto context = std::make_shared<Context>();
    k_sem_init(&context->entered, 0, 1);

    // Holds the queue until the session failed, then stores; the supervisor's run comes after it.
    rig.config_work_queue->Run([context, writer = rig.log_writer_service, manager = rig.logging_configuration_manager] {
        k_sem_give(&context->entered);
        context->saw_failure = WaitFor(
            [&] {
                return writer->HasFailed();
            },
            2000
        );
        context->is_stored = TryStoreLoggingConfiguration(*manager, 50);
    });
    zassert_equal(k_sem_take(&context->entered, K_SECONDS(2)), 0, "the job did not run");
    failing.probes->Current()->refuse_next_reading = true;

    zassert_true(recorder.WaitForEventCount(3), "the failure and the stop must each publish a status");
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    k_msleep(50);

    zassert_true(context->saw_failure.load(), "the session must have failed while the job held the queue");
    zassert_true(context->is_stored.load(), "the configuration must have been stored on the queue");
    zassert_true(rig.log_writer_service->IsStopped(), "the failed session must be stopped, not restarted");
    zassert_equal(CountLogFiles(*rig.fs_service), 1, "no file may be opened for the stored configuration");

    const auto events = recorder.GetEvents();
    zassert_equal(events.size(), 3, "start, failure, stop (%zu events)", events.size());
    zassert_true(events[1].reason == LoggingStatusReason::WriteFailed, "the failure");
    zassert_true(events[2].reason == LoggingStatusReason::Stopped, "the stop");
}

// A configuration stored before a start that the same run applies: the start opens its file with
// it, so no restart follows.
ZTEST(log_writer_supervisor, test_AStartInTheRunOfAStoredConfigurationOpensOneFile) {
    auto rig = MakeLogWriterRig();
    const StatusRecorder recorder;
    auto& supervisor = AttachSupervisor(rig);

    // One job does both, so the task's next run finds the restart due and the start pending.
    const auto is_stored = std::make_shared<std::atomic<bool>>(false);
    rig.config_work_queue->Run([&supervisor, is_stored, manager = rig.logging_configuration_manager] {
        *is_stored = TryStoreLoggingConfiguration(*manager, 50);
        supervisor.RequestStart(ILoggingControl::kAllUnits);
    });

    zassert_true(recorder.WaitForEventCount(1), "the start must publish its status");
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    k_msleep(100);

    zassert_true(is_stored->load(), "the configuration must have been stored on the queue");
    zassert_true(rig.log_writer_service->IsRunning(), "the start must run");
    zassert_equal(
        recorder.GetEvents().size(), 1, "no restart may follow the start (%zu events)", recorder.GetEvents().size()
    );
    zassert_equal(CountLogFiles(*rig.fs_service), 1, "one file");

    supervisor.RequestStop(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(2), "the stop must publish its status");
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
}

// A pending run is dropped, not waited for, and the subscription goes with the supervisor.
ZTEST(log_writer_supervisor, test_DestroyingWithAPendingRequestStartsNothing) {
    auto rig = MakeLogWriterRig();
    const StatusRecorder recorder;
    auto& supervisor = AttachSupervisor(rig);

    const auto& channel = LoggingEventsChannel::GetInstance();
    const auto generation = channel.GetSubscriptionGeneration();

    zassert_true(Gate().Close(*rig.config_work_queue), "the configuration work queue could not be held");
    supervisor.RequestStart(ILoggingControl::kAllUnits);

    const int64_t start_ms = k_uptime_get();
    ReleaseSupervisor();
    const int64_t elapsed_ms = k_uptime_get() - start_ms;
    zassert_true(
        elapsed_ms < kNoWaitMs, "the destructor waited %lld ms for a run that had not begun",
        static_cast<long long>(elapsed_ms)
    );
    zassert_not_equal(channel.GetSubscriptionGeneration(), generation, "the destructor must unsubscribe");

    Gate().Open();
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    k_msleep(50);

    zassert_true(rig.log_writer_service->IsStopped(), "the pending start must not run");
    zassert_equal(CountLogFiles(*rig.fs_service), 0, "no file may be opened");
    zassert_equal(recorder.GetEvents().size(), 0, "nothing may be published");
}

// The destructor waits for a run in progress, and a configuration stored afterwards restarts
// nothing: the updated handler went with the supervisor. The dependent holds the writer and the
// wanted state, not the supervisor, so it still stops and starts the writer afterwards.
ZTEST(log_writer_supervisor, test_DestroyingWhileTheTaskRunsWaitsForIt) {
    auto rig = MakeLogWriterRig();
    const StatusRecorder recorder;
    auto& supervisor = AttachSupervisor(rig);
    const auto dependent = supervisor.GetDependentService();

    // A factory that takes its time, so the destructor arrives in the middle of the start.
    const auto entered = std::make_shared<k_sem>();
    k_sem_init(entered.get(), 0, 1);
    rig.log_writer_service->SetLoggerFactory([entered, inner = rig.logger_factory] {
        k_sem_give(entered.get());
        k_msleep(ProbeLogger::kHoldMs);

        return inner();
    });

    supervisor.RequestStart(ILoggingControl::kAllUnits);
    zassert_equal(k_sem_take(entered.get(), K_SECONDS(2)), 0, "the start did not run");

    const int64_t start_ms = k_uptime_get();
    ReleaseSupervisor();
    const int64_t elapsed_ms = k_uptime_get() - start_ms;
    zassert_true(
        elapsed_ms >= kNoWaitMs, "the destructor returned after %lld ms, before the start finished",
        static_cast<long long>(elapsed_ms)
    );
    zassert_true(rig.log_writer_service->IsRunning(), "the start in progress must have finished");
    zassert_true(recorder.WaitForEventCount(1), "the start must publish its status");

    StoreLoggingConfiguration(rig, 50);
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    k_msleep(50);
    zassert_true(rig.log_writer_service->IsRunning(), "the writer must keep running");
    zassert_equal(CountLogFiles(*rig.fs_service), 1, "no restart may open a file");
    zassert_equal(recorder.GetEvents().size(), 1, "nothing may be published (%zu events)", recorder.GetEvents().size());

    struct Results {
        std::atomic<bool> is_stopped = false;
        std::atomic<bool> is_started = false;
    };
    const auto results = std::make_shared<Results>();
    rig.config_work_queue->Run([dependent, results] {
        results->is_stopped = dependent->Stop();
        results->is_started = dependent->Start();
    });
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");

    zassert_true(results->is_stopped.load(), "the dependent must stop the writer");
    zassert_true(results->is_started.load(), "the dependent must start the writer again: logging is still wanted");
    zassert_true(rig.log_writer_service->IsRunning(), "the writer must run again");
    zassert_equal(CountLogFiles(*rig.fs_service), 2, "the restart opens a second file");

    rig.config_work_queue->Run([dependent, results] {
        results->is_stopped = dependent->Stop();
    });
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    zassert_true(rig.log_writer_service->IsStopped(), "the dependent must stop the writer");
}

// The channel snapshots its subscribers before it calls them, so the supervisor's handler can still
// be called after the destructor unsubscribed it. The guard makes that call a no-op.
ZTEST(log_writer_supervisor, test_AStatusEventInFlightDuringDestructionDoesNothing) {
    FailingRig failing;
    auto& rig = failing.rig;
    StatusBlocker blocker;
    auto& supervisor = AttachSupervisor(rig);
    UseProbes(failing);

    supervisor.RequestStart(ILoggingControl::kAllUnits);
    zassert_true(
        WaitFor(
            [&] {
                return supervisor.IsActive();
            },
            2000
        ),
        "the start must be reported"
    );

    blocker.Arm();
    failing.probes->Current()->refuse_next_reading = true;
    zassert_true(blocker.WaitUntilBlocking(), "the failure event must reach the blocker");

    ReleaseSupervisor();
    blocker.Unblock();
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    k_msleep(200);

    zassert_true(
        rig.log_writer_service->IsRunning(), "nothing may stop the failed session once the supervisor is gone"
    );
    zassert_true(rig.log_writer_service->HasFailed(), "the session has failed");

    zassert_true(rig.log_writer_service->Stop(), "the failed session must stop");
}

// The supervisor clears the logging manager's updated handler when it goes, while a store on the
// configuration work queue may be calling it: the clear returns at once, the call in progress
// finishes on its own reference to the handler, and the next store calls nothing.
ZTEST(log_writer_supervisor, test_AHandlerClearedDuringItsCallFinishesThatCall) {
    auto rig = MakeLogWriterRig();
    auto& manager = *rig.logging_configuration_manager;

    struct Signals {
        k_sem entered;
        k_sem release;
        std::atomic<int> calls = 0;
        std::atomic<bool> is_stored = false;
    };
    const auto signals = std::make_shared<Signals>();
    k_sem_init(&signals->entered, 0, 1);
    k_sem_init(&signals->release, 0, 1);

    // Owned by the handler alone, so it lives exactly as long as the handler does.
    auto sentinel = std::make_shared<int>(0);
    const std::weak_ptr<int> weak_sentinel = sentinel;
    manager.RegisterConfigurationUpdatedHandler([signals, sentinel = std::move(sentinel)] {
        signals->calls++;
        k_sem_give(&signals->entered);
        k_sem_take(&signals->release, K_SECONDS(5));
    });
    zassert_false(weak_sentinel.expired(), "the registered handler holds the sentinel");

    rig.config_work_queue->Run([signals, manager = rig.logging_configuration_manager] {
        signals->is_stored = TryStoreLoggingConfiguration(*manager, 50);
    });
    zassert_equal(k_sem_take(&signals->entered, K_SECONDS(2)), 0, "the store must call the handler");

    const int64_t start_ms = k_uptime_get();
    manager.RegisterConfigurationUpdatedHandler(nullptr);
    const int64_t elapsed_ms = k_uptime_get() - start_ms;
    zassert_true(
        elapsed_ms < kNoWaitMs, "clearing the handler waited %lld ms for its call", static_cast<long long>(elapsed_ms)
    );
    zassert_false(weak_sentinel.expired(), "the call in progress must keep its handler alive");

    k_sem_give(&signals->release);
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    zassert_true(signals->is_stored.load(), "the configuration must have been stored");
    zassert_true(weak_sentinel.expired(), "the handler must go once its call returned");

    StoreLoggingConfiguration(rig, 80);
    zassert_equal(signals->calls.load(), 1, "a cleared handler must not be called again");
}

// A supervisor whose Initialize() failed (here its queue never ran, so it has no task) acts on no
// request and, when it goes, leaves the manager's updated handler alone: it never registered one.
ZTEST(log_writer_supervisor, test_ASupervisorThatFailedToInitializeDoesNothing) {
    auto rig = MakeLogWriterRig();
    const StatusRecorder recorder;

    const auto handler_calls = std::make_shared<std::atomic<int>>(0);
    rig.logging_configuration_manager->RegisterConfigurationUpdatedHandler([handler_calls] {
        (*handler_calls)++;
    });

    // Never initialized, so CreateTask() throws.
    const auto idle_queue = std::make_shared<WorkQueueThread>("idle_queue", 2048, 5);
    SupervisorSlot() = std::make_shared<LogWriterSupervisor>(
        idle_queue, rig.log_writer_service, rig.logging_configuration_manager, MakeSensorsManager(rig), kTestToolId
    );
    auto& supervisor = *SupervisorSlot();
    zassert_false(supervisor.Initialize(), "Initialize() must fail without a running queue");

    supervisor.RequestStart(ILoggingControl::kAllUnits);
    supervisor.RequestStop(ILoggingControl::kAllUnits);
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");
    k_msleep(50);
    zassert_true(rig.log_writer_service->IsStopped(), "a request must do nothing without a successful Initialize()");
    zassert_equal(CountLogFiles(*rig.fs_service), 0, "no file may be opened");
    zassert_equal(recorder.GetEvents().size(), 0, "nothing may be published");
    zassert_false(supervisor.IsActive(), "nothing runs");

    ReleaseSupervisor();
    StoreLoggingConfiguration(rig, 50);
    zassert_equal(handler_calls->load(), 1, "the destructor must leave a handler it did not register");

    rig.logging_configuration_manager->RegisterConfigurationUpdatedHandler(nullptr);
}

// The supervisor's own factory: the tool ID it was given, the version, product ID and board from
// Kconfig, the device ID from its provider, the stored sensors and their configuration's CRC.
ZTEST(log_writer_supervisor, test_TheLoggerNamesTheToolTheDeviceAndTheSensors) {
    auto rig = MakeLogWriterRig();
    const StatusRecorder recorder;

    const auto sensors_manager = MakeSensorsManager(rig);
    const std::vector<std::shared_ptr<Sensor>> sensors = { MakeVirtualSensor("logged_virtual") };
    zassert_true(sensors_manager->Update(sensors), "the sensors were not stored");

    auto& supervisor = AttachSupervisor(rig, sensors_manager, true, [] {
        return uint8_t{ 7 };
    });
    EnableSensorLogging(rig, sensors);

    supervisor.RequestStart(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(1), "the start must publish its status");
    supervisor.RequestStop(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(2), "the stop must publish its status");
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");

    const auto files = ReadLogFiles(*rig.fs_service);
    zassert_equal(files.size(), 1, "one file (found %zu)", files.size());
    const auto& content = files[0].content;
    AssertFinalizedFile(content, "the file");

    const auto info = sensors_manager->GetCborConfigurationInfo();
    zassert_true(info.is_applied && info.crc != 0, "the stored sensors configuration must be applied");

    const std::string version = std::to_string(CONFIG_EERIE_LEAP_SW_VERSION_MAJOR) + "."
        + std::to_string(CONFIG_EERIE_LEAP_SW_VERSION_MINOR) + "." + std::to_string(CONFIG_EERIE_LEAP_SW_VERSION_PATCH);

    zassert_true(ContainsText(content, std::string("<tool_id>") + kTestToolId + "</tool_id>"), "the tool ID");
    zassert_true(ContainsText(content, "<tool_version>" + version + "</tool_version>"), "the tool version");
    zassert_true(ContainsText(content, Property("device_name", CONFIG_BOARD)), "the board");
    zassert_true(
        ContainsText(content, Property("product_id", std::to_string(CONFIG_EERIE_LEAP_PRODUCT_ID))), "the product ID"
    );
    zassert_true(ContainsText(content, Property("device_id", "7")), "the device ID");
    zassert_true(
        ContainsText(content, Property("hw_version", std::to_string(SystemConfiguration::hw_version))),
        "the hardware version"
    );
    zassert_true(
        ContainsText(content, Property("sw_version", std::to_string(SystemConfiguration::sw_version))),
        "the software version"
    );
    zassert_true(
        ContainsText(content, Property("sensors_configuration_hash", std::to_string(info.crc))),
        "the sensors configuration CRC"
    );
    zassert_true(ContainsText(content, "logged_virtual"), "the stored sensor");
}

// A stored sensors configuration that was rejected at boot is not what runs, so its CRC is not
// logged; without a provider the device ID is 0.
ZTEST(log_writer_supervisor, test_ARejectedSensorsConfigurationIsLoggedWithoutAHash) {
    auto rig = MakeLogWriterRig();
    const StatusRecorder recorder;

    const std::vector<uint8_t> garbage(16, 0xFF);
    zassert_true(
        rig.fs_service->WriteFile("config/sensors_config.cbor", garbage.data(), garbage.size()),
        "the garbage configuration could not be written"
    );
    const auto sensors_manager = MakeSensorsManager(rig);
    zassert_true(sensors_manager->IsStoredConfigurationRejected(), "the garbage must be rejected");

    auto& supervisor = AttachSupervisor(rig, sensors_manager, true);

    supervisor.RequestStart(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(1), "the start must publish its status");
    supervisor.RequestStop(ILoggingControl::kAllUnits);
    zassert_true(recorder.WaitForEventCount(2), "the stop must publish its status");
    zassert_true(FlushConfigQueue(rig), "the configuration work queue did not run");

    const auto files = ReadLogFiles(*rig.fs_service);
    zassert_equal(files.size(), 1, "one file (found %zu)", files.size());
    zassert_true(ContainsText(files[0].content, Property("sensors_configuration_hash", "0")), "an unknown hash is 0");
    zassert_true(ContainsText(files[0].content, Property("device_id", "0")), "no provider, device ID 0");
    zassert_true(ContainsText(files[0].content, std::string("<tool_id>") + kTestToolId + "</tool_id>"), "the tool ID");
}
