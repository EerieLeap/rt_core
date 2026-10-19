#include <memory>
#include <stdexcept>
#include <vector>

#include <zephyr/ztest.h>

#include "domain/logging_domain/control/remote_logging_control.h"

#include "logging_control_test_support.h"

using namespace logging_control_test;

using eerie_leap::domain::logging_domain::control::RemoteLoggingControl;

namespace {

using Starts = std::vector<bool>;

// The control under test on the binary's configuration queue, with fakes in place of the COM service.
struct Fixture {
    std::shared_ptr<WorkQueueThread> queue = TestConfigQueue();
    std::shared_ptr<FakeCommandSender> sender = std::make_shared<FakeCommandSender>();
    std::shared_ptr<FakeRestartMonitor> monitor = std::make_shared<FakeRestartMonitor>();
    std::shared_ptr<FakeNetworkInfo> network = std::make_shared<FakeNetworkInfo>();
    std::unique_ptr<RemoteLoggingControl> control;

    void Build() {
        control = std::make_unique<RemoteLoggingControl>(queue, sender, monitor);
        zassert_true(control->Initialize(), "The control did not initialize.");
    }

    // The same over the fake device table, as a remote unit (the default) or as the network half of a writer.
    void BuildWithNetwork(RemoteLoggingControl::Mode mode = RemoteLoggingControl::Mode::Remote) {
        control = std::make_unique<RemoteLoggingControl>(queue, sender, monitor, network, mode);
        zassert_true(control->Initialize(), "The control did not initialize.");
    }

    // Everything queued so far has run.
    void Flush() {
        zassert_true(FlushQueue(*queue), "The configuration queue did not run.");
    }
};

Fixture*& Current() {
    static Fixture* fixture = nullptr;

    return fixture;
}

void* SetUp() {
    return SetUpChannels();
}

void Before(void*) {
    Current() = new Fixture();
}

void After(void*) {
    Gate().Open();

    if(Current() != nullptr) {
        // A send a failed case left holding would keep the control's destructor waiting.
        Current()->sender->Release();
        Current()->control.reset();
        delete Current();
        Current() = nullptr;
    }

    // The bus delivers what the case published before the next one subscribes.
    k_msleep(20);
}

Fixture& F() {
    return *Current();
}

} // namespace

ZTEST_SUITE(remote_logging_control, NULL, SetUp, Before, After, NULL);

ZTEST(remote_logging_control, test_ARequestSendsFromTheConfigurationWorkQueue) {
    F().Build();

    F().control->RequestStart(ALL_UNITS);
    zassert_true(F().sender->WaitForSends(1), "A start request must be sent");

    F().control->RequestStop(ALL_UNITS);
    zassert_true(F().sender->WaitForSends(2), "A stop request must be sent");

    const auto sends = F().sender->Sends();
    zassert_true(sends[0].is_start);
    zassert_false(sends[1].is_start);
    for(const auto& send : sends) {
        zassert_equal(send.thread, QueueThread(*F().queue), "The send runs on the configuration work queue");
        zassert_not_equal(send.thread, k_current_get(), "and not on the caller's thread");
    }
}

ZTEST(remote_logging_control, test_RequestsBeforeTheTaskRunsSendOneCommand) {
    F().Build();

    zassert_true(Gate().Close(*F().queue), "The queue must be held");

    // The requests return at once, whatever the queue is doing.
    const int64_t start_ms = k_uptime_get();
    F().control->RequestStart(ALL_UNITS);
    F().control->RequestStop(ALL_UNITS);
    F().control->RequestStart(ALL_UNITS);
    zassert_true(k_uptime_get() - start_ms < NO_WAIT_MS, "Requests must not wait for the send");

    Gate().Open();
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true }), "Start, stop, start is one command: the latest");

    zassert_true(Gate().Close(*F().queue));
    F().control->RequestStart(ALL_UNITS);
    F().control->RequestStop(ALL_UNITS);
    Gate().Open();
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true, false }), "Start, stop is a stop");
}

ZTEST(remote_logging_control, test_AWantedStartIsSentAgainWhenALoggingUnitAppears) {
    F().Build();

    // No logging unit is online when the start is wanted.
    PublishStatus(false, LoggingStatusReason::Unavailable);
    F().control->RequestStart(ALL_UNITS);
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true }));

    PublishStatus(false, LoggingStatusReason::Stopped);
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true, true }), "A logging unit appeared: the start is sent again");

    PublishStatus(false, LoggingStatusReason::Stopped);
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true, true }), "Stopped, then Stopped again, is no appearance");

    // The unit goes away and comes back: once more.
    PublishStatus(false, LoggingStatusReason::Unavailable);
    PublishStatus(false, LoggingStatusReason::Stopped);
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true, true, true }), "Each appearance is one re-assertion");
}

// A request and a logging unit's appearance taken in one run: the request's send is the re-assertion.
ZTEST(remote_logging_control, test_AReassertionInTheRunOfARequestIsThatRequest) {
    F().Build();

    PublishStatus(false, LoggingStatusReason::Unavailable);

    zassert_true(Gate().Close(*F().queue), "The queue must be held");
    F().control->RequestStart(ALL_UNITS);
    PublishStatus(false, LoggingStatusReason::Stopped);
    Gate().Open();
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true }), "One start for the request and the appearance");

    // The unit goes away, and a stop is requested in the run that sees it come back.
    PublishStatus(false, LoggingStatusReason::Unavailable);
    zassert_true(Gate().Close(*F().queue), "The queue must be held");
    PublishStatus(false, LoggingStatusReason::Stopped);
    F().control->RequestStop(ALL_UNITS);
    Gate().Open();
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true, false }), "The stop is sent and no start follows it");
}

ZTEST(remote_logging_control, test_NoStartIsSentAgainAfterLoggingRan) {
    F().Build();

    PublishStatus(false, LoggingStatusReason::Unavailable);
    F().control->RequestStart(ALL_UNITS);
    F().Flush();

    PublishStatus(true);
    PublishStatus(false, LoggingStatusReason::Stopped);
    F().Flush();

    zassert_true(F().sender->Starts() == Starts({ true }), "A unit that logged and stopped stands as it stopped");
}

ZTEST(remote_logging_control, test_NoStartIsSentAgainAfterAStop) {
    F().Build();

    F().control->RequestStart(ALL_UNITS);
    F().Flush();
    F().control->RequestStop(ALL_UNITS);
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true, false }));

    PublishStatus(false, LoggingStatusReason::Unavailable);
    PublishStatus(false, LoggingStatusReason::Stopped);
    F().Flush();

    zassert_true(F().sender->Starts() == Starts({ true, false }), "A stop ends what was wanted");
}

ZTEST(remote_logging_control, test_NoStartIsSentAgainAfterAFailure) {
    F().Build();

    F().control->RequestStart(ALL_UNITS);
    F().Flush();

    PublishStatus(false, LoggingStatusReason::Unavailable);
    PublishStatus(false, LoggingStatusReason::NoSpace);
    PublishStatus(false, LoggingStatusReason::Stopped);
    F().Flush();

    zassert_true(F().sender->Starts() == Starts({ true }), "A failure stands until the switch moves");
}

ZTEST(remote_logging_control, test_TheFirstStatusSendsNothing) {
    F().Build();

    F().control->RequestStart(ALL_UNITS);
    F().Flush();

    // No predecessor: the unit was there, or this is the first we hear of it, and the start was sent.
    PublishStatus(false, LoggingStatusReason::Stopped);
    F().Flush();

    zassert_true(F().sender->Starts() == Starts({ true }));
}

ZTEST(remote_logging_control, test_NothingIsSentAgainWithoutARequest) {
    F().Build();

    PublishStatus(false, LoggingStatusReason::Unavailable);
    PublishStatus(false, LoggingStatusReason::Stopped);
    F().Flush();

    zassert_equal(F().sender->Sends().size(), 0, "Nothing was wanted");
}

ZTEST(remote_logging_control, test_AnEventWithoutAStatusIsIgnored) {
    F().Build();

    PublishStatus(false, LoggingStatusReason::Unavailable);
    F().control->RequestStart(ALL_UNITS);
    F().Flush();

    // The fence events of the blockers carry no IsActive.
    LoggingEventsChannel::GetInstance().Publish(
        { .source_id = FENCE_SOURCE_ID, .type = LoggingEventType::StatusUpdated }
    );
    PublishStatus(false, LoggingStatusReason::Stopped);
    F().Flush();

    zassert_true(
        F().sender->Starts() == Starts({ true, true }), "An event without IsActive does not reset the predecessor"
    );
}

ZTEST(remote_logging_control, test_IsActiveFollowsTheStatus) {
    F().Build();

    zassert_false(F().control->IsActive(), "Nothing logs before a status");

    PublishStatus(true);
    zassert_true(F().control->IsActive());

    zassert_equal(F().control->GetReason(), LoggingStatusReason::Stopped, "Active has no reason");

    PublishStatus(false, LoggingStatusReason::Stopped);
    zassert_false(F().control->IsActive());

    PublishStatus(false, LoggingStatusReason::Unavailable);
    zassert_equal(F().control->GetReason(), LoggingStatusReason::Unavailable, "The reason follows the last status");
}

// The restart monitor's report, from the CDMP work queue in the real wiring: the logging unit
// restarted and shares an idle status again.
ZTEST(remote_logging_control, test_ARestartedLoggingUnitGetsTheWantedStartAgain) {
    F().Build();

    F().control->RequestStart(ALL_UNITS);
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true }));

    F().monitor->Report(1);
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true, true }), "The restarted unit lost the start: it is sent again");

    const auto sends = F().sender->Sends();
    zassert_equal(sends[1].thread, QueueThread(*F().queue), "from the configuration work queue");
    zassert_not_equal(sends[1].thread, k_current_get(), "and not on the reporting thread");
}

ZTEST(remote_logging_control, test_ARestartWithoutAWantedStartSendsNothing) {
    F().Build();

    F().monitor->Report(1);
    F().Flush();
    zassert_equal(F().sender->Sends().size(), 0, "Nothing was wanted");

    F().control->RequestStart(ALL_UNITS);
    F().Flush();
    F().control->RequestStop(ALL_UNITS);
    F().Flush();
    F().monitor->Report(1);
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true, false }), "A stop ends what was wanted");
}

ZTEST(remote_logging_control, test_EachRestartIsReassertedOnce) {
    F().Build();

    F().control->RequestStart(ALL_UNITS);
    F().Flush();

    F().monitor->Report(1);
    F().Flush();
    F().monitor->Report(1);
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true, true, true }), "Each restart is one re-assertion");

    // Reports taken in one run send one start to each unit they name, however often it is named.
    zassert_true(Gate().Close(*F().queue), "The queue must be held");
    F().monitor->Report(1);
    F().monitor->Report(3);
    F().monitor->Report(1);
    Gate().Open();
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true, true, true, true, true }), "Two units: one start each");
    zassert_equal(F().sender->SentTo(1), 3U, "and the unit named twice got one");
    zassert_equal(F().sender->SentTo(3), 1U);
}

// A request and a restart taken in one run: the request's send is the re-assertion.
ZTEST(remote_logging_control, test_ARestartInTheRunOfARequestIsThatRequest) {
    F().Build();

    zassert_true(Gate().Close(*F().queue), "The queue must be held");
    F().control->RequestStart(ALL_UNITS);
    F().monitor->Report(1);
    Gate().Open();
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true }), "One start for the request and the restart");

    // A stop requested in the run that sees the restart is the only thing sent.
    zassert_true(Gate().Close(*F().queue), "The queue must be held");
    F().monitor->Report(1);
    F().control->RequestStop(ALL_UNITS);
    Gate().Open();
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true, false }), "The stop is sent and no start follows it");
}

// Both signals for one cause, as a restarted unit that announces its record late produces when
// another unit still reports: one start.
ZTEST(remote_logging_control, test_ARestartAndAnAppearanceInOneRunSendOneStart) {
    F().Build();

    PublishStatus(false, LoggingStatusReason::Unavailable);
    F().control->RequestStart(ALL_UNITS);
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true }));

    zassert_true(Gate().Close(*F().queue), "The queue must be held");
    PublishStatus(false, LoggingStatusReason::Stopped);
    F().monitor->Report(1);
    Gate().Open();
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true, true }), "One start for the appearance and the restart");
}

ZTEST(remote_logging_control, test_TheRestartHandlerIsSetByInitializeAndClearedOnDestruction) {
    F().control = std::make_unique<RemoteLoggingControl>(F().queue, F().sender, F().monitor);
    zassert_false(F().monitor->HasHandler(), "Nothing listens before Initialize()");
    zassert_true(F().control->Initialize());
    zassert_true(F().monitor->HasHandler(), "Initialize() sets the handler");

    F().control.reset();
    zassert_false(F().monitor->HasHandler(), "and the destructor clears it");

    // A control that does not initialize leaves no handler either.
    const auto idle_queue = std::make_shared<WorkQueueThread>("idle_queue", 2048, 5);
    auto failed = std::make_unique<RemoteLoggingControl>(idle_queue, F().sender, F().monitor);
    zassert_false(failed->Initialize(), "Initialize() must fail without a running queue");
    zassert_false(F().monitor->HasHandler());
    failed.reset();
    zassert_false(F().monitor->HasHandler());
}

// A report the monitor had already copied when the control went: it reaches a detached guard.
ZTEST(remote_logging_control, test_ARestartReportInFlightDuringDestructionDoesNothing) {
    F().Build();

    F().control->RequestStart(ALL_UNITS);
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true }));

    const auto handler = F().monitor->CopyHandler();
    zassert_not_null(handler.get(), "The control set its handler");

    F().control.reset();

    (*handler)(1);
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true }), "A detached handler sends nothing");
}

// The monitor allocates when it takes the handler, so it can refuse. Nothing is left behind, and
// the control can initialize again. The status subscription is live meanwhile, so what a status
// does before the refusal is part of "nothing".
ZTEST(remote_logging_control, test_AMonitorThatCannotTakeItsHandlerFailsInitialize) {
    F().control = std::make_unique<RemoteLoggingControl>(F().queue, F().sender, F().monitor);

    // A logging unit logs while the control initializes.
    F().monitor->FailNextSet();
    F().monitor->OnSet([] {
        PublishStatus(true);
    });
    zassert_false(F().control->Initialize(), "Initialize() must fail when the monitor refuses");
    zassert_false(F().monitor->HasHandler());
    zassert_false(F().control->IsActive(), "what the failed attempt saw is forgotten");

    // No logging unit is online meanwhile.
    F().monitor->FailNextSet();
    F().monitor->OnSet([] {
        PublishStatus(false, LoggingStatusReason::Unavailable);
    });
    zassert_false(F().control->Initialize());
    F().monitor->OnSet(nullptr);

    // No subscription is left to see a status, and no task to send from.
    PublishStatus(true);
    zassert_false(F().control->IsActive(), "The status subscription was withdrawn");
    F().control->RequestStart(ALL_UNITS);
    PublishStatus(false, LoggingStatusReason::Unavailable);
    PublishStatus(false, LoggingStatusReason::Stopped);
    F().Flush();
    zassert_equal(F().sender->Sends().size(), 0, "A control that is not initialized sends nothing");

    zassert_true(F().control->Initialize(), "A later attempt starts clean");
    F().control->RequestStart(ALL_UNITS);
    F().Flush();
    PublishStatus(false, LoggingStatusReason::Stopped);
    F().Flush();
    zassert_true(
        F().sender->Starts() == Starts({ true }), "The Unavailable of the failed attempt is forgotten: no predecessor"
    );

    PublishStatus(false, LoggingStatusReason::Unavailable);
    PublishStatus(false, LoggingStatusReason::Stopped);
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true, true }), "and it sees the status");

    F().monitor->Report(1);
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true, true, true }), "and the restarts");
}

// A logging unit appears while the monitor refuses its handler: the status handler schedules the task,
// and a queue held by another job keeps that run pending. The failed Initialize() cancels the run
// before the task goes, because the queue still lists the work item inside the dropped task, and the
// next Initialize() makes a new task over it. The case pins that sequence. Without the cancel it still
// passes on a build without a sanitizer: the dropped task still answers, and the flush job relinks
// the queue's list.
ZTEST(remote_logging_control, test_AFailedInitializeCancelsTheRunItScheduled) {
    F().control = std::make_unique<RemoteLoggingControl>(F().queue, F().sender, F().monitor);

    zassert_true(Gate().Close(*F().queue), "The queue must be held");
    F().monitor->FailNextSet();
    F().monitor->OnSet([] {
        PublishStatus(false, LoggingStatusReason::Unavailable);
        PublishStatus(false, LoggingStatusReason::Stopped);
    });
    zassert_false(F().control->Initialize(), "Initialize() must fail when the monitor refuses");
    F().monitor->OnSet(nullptr);

    // The queue is still held, so the run of the task that was dropped is not over.
    zassert_true(F().control->Initialize(), "A later attempt succeeds");
    F().control->RequestStart(ALL_UNITS);

    Gate().Open();
    F().Flush();
    zassert_true(
        F().sender->Starts() == Starts({ true }), "The request is sent once, and the dropped run left nothing behind"
    );
}

ZTEST(remote_logging_control, test_ARequestBeforeInitializeSendsNothing) {
    F().control = std::make_unique<RemoteLoggingControl>(F().queue, F().sender);

    F().control->RequestStart(ALL_UNITS);
    F().control->RequestStop(ALL_UNITS);
    F().Flush();

    zassert_equal(F().sender->Sends().size(), 0, "A control that is not initialized has no task to send from");
}

// The task can be created only on a queue that runs: Initialize() fails, and the control, which has no
// task then, is still safe to ask and to destroy.
ZTEST(remote_logging_control, test_AControlThatFailedToInitializeDoesNothing) {
    // Never initialized, so CreateTask() throws.
    const auto idle_queue = std::make_shared<WorkQueueThread>("idle_queue", 2048, 5);
    auto control = std::make_unique<RemoteLoggingControl>(idle_queue, F().sender);

    zassert_false(control->Initialize(), "Initialize() must fail without a running queue");

    control->RequestStart(ALL_UNITS);
    control->RequestStop(ALL_UNITS);
    F().Flush();
    zassert_equal(F().sender->Sends().size(), 0, "A request must do nothing without a successful Initialize()");
    zassert_false(control->IsActive());

    // Without a task to cancel and without a subscription to withdraw.
    control.reset();
}

ZTEST(remote_logging_control, test_ANullArgumentIsRefused) {
    bool is_queue_refused = false;
    try {
        RemoteLoggingControl control(nullptr, F().sender);
    } catch(const std::invalid_argument&) {
        is_queue_refused = true;
    }
    zassert_true(is_queue_refused, "A control needs a queue");

    bool is_sender_refused = false;
    try {
        RemoteLoggingControl control(F().queue, nullptr);
    } catch(const std::invalid_argument&) {
        is_sender_refused = true;
    }
    zassert_true(is_sender_refused, "and a sender");
}

ZTEST(remote_logging_control, test_DestroyingWithAPendingSendSendsNothing) {
    F().Build();

    zassert_true(Gate().Close(*F().queue), "The queue must be held");
    F().control->RequestStart(ALL_UNITS);
    F().control.reset();

    Gate().Open();
    F().Flush();

    zassert_equal(F().sender->Sends().size(), 0, "A destroyed control sends no pending command");
}

// The destructor cancels the task with a wait: a control that did not would be freed, and with it the
// last reference to the sender and the COM service behind it, under a send in progress.
ZTEST(remote_logging_control, test_DestroyingWhileASendRunsWaitsForIt) {
    F().Build();

    F().sender->HoldSends();
    F().control->RequestStart(ALL_UNITS);
    zassert_true(F().sender->WaitUntilHeld(), "The send must be running");

    // Destroyed on another thread, which has to wait for the send on the configuration work queue;
    // the destructor must not run on that queue.
    struct Destruction {
        k_sem done{};
        std::unique_ptr<RemoteLoggingControl> control;
    };
    const auto destruction = std::make_shared<Destruction>();
    k_sem_init(&destruction->done, 0, 1);
    destruction->control = std::move(F().control);

    WorkQueueThread helper("control_destroyer", 4096, 5);
    zassert_true(helper.Initialize());
    helper.Run([destruction] {
        destruction->control.reset();
        k_sem_give(&destruction->done);
    });

    zassert_not_equal(
        k_sem_take(&destruction->done, Ms(NO_WAIT_MS)), 0, "The destructor must wait for the send in progress"
    );

    F().sender->Release();
    zassert_equal(k_sem_take(&destruction->done, Ms(WAIT_MS)), 0, "and return once it finishes");

    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true }), "One send in all");
}

ZTEST(remote_logging_control, test_AStatusInFlightDuringDestructionDoesNothing) {
    // Made before the control, so its handler runs first in a dispatch and holds the bus thread there.
    LoggingBlocker blocker(LoggingEventsChannel::GetInstance(), LoggingEventType::StatusUpdated);

    F().Build();

    PublishStatus(false, LoggingStatusReason::Unavailable);
    F().control->RequestStart(ALL_UNITS);
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true }));

    blocker.Arm();
    PublishStatus(false, LoggingStatusReason::Stopped, true);
    zassert_true(blocker.WaitUntilBlocking(), "The bus thread is inside the dispatch");

    // The bus has the control's handler in its snapshot of this dispatch, and the status would
    // re-assert the start.
    F().control.reset();

    blocker.Release();
    zassert_true(blocker.Fence(), "The dispatch finished");
    F().Flush();

    zassert_true(F().sender->Starts() == Starts({ true }), "A detached handler sends nothing");
}

// ======== A LOGGING command from another unit (HandleLoggingCommand, L9) ========

// The stop another unit sent to every unit drops the wanted start, so no re-assertion sends a start
// that undoes it.
ZTEST(remote_logging_control, test_AStopFromAnotherUnitDropsTheWantedStart) {
    F().Build();

    PublishStatus(false, LoggingStatusReason::Unavailable);
    F().control->RequestStart(ALL_UNITS);
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true }));

    zassert_true(F().control->HandleLoggingCommand(false), "A stop is served");
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true }), "and sends nothing: the other unit sent it to every unit");

    PublishStatus(false, LoggingStatusReason::Stopped);
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true }), "No logging unit appearing re-asserts the start");

    F().monitor->Report(1);
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true }), "and neither does a restart the monitor reports");
}

ZTEST(remote_logging_control, test_AStopFromAnotherUnitDropsAStartNotYetSent) {
    F().Build();

    zassert_true(Gate().Close(*F().queue), "The queue must be held");
    F().control->RequestStart(ALL_UNITS);
    zassert_true(F().control->HandleLoggingCommand(false));
    Gate().Open();
    F().Flush();

    zassert_equal(F().sender->Sends().size(), 0U, "The stop is the later request: the start is never sent");
}

ZTEST(remote_logging_control, test_AStopFromAnotherUnitKeepsAStopNotYetSent) {
    F().Build();

    zassert_true(Gate().Close(*F().queue), "The queue must be held");
    F().control->RequestStop(ALL_UNITS);
    zassert_true(F().control->HandleLoggingCommand(false));
    Gate().Open();
    F().Flush();

    zassert_true(F().sender->Starts() == Starts({ false }), "This unit's own update stop is still sent");
}

ZTEST(remote_logging_control, test_AStartFromAnotherUnitIsNotServed) {
    F().Build();

    PublishStatus(false, LoggingStatusReason::Unavailable);
    F().control->RequestStart(ALL_UNITS);
    F().Flush();

    zassert_false(F().control->HandleLoggingCommand(true), "This unit has no writer: FAILURE");
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true }), "Nothing is sent");

    PublishStatus(false, LoggingStatusReason::Stopped);
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true, true }), "and the wanted start is as it was");
}

ZTEST(remote_logging_control, test_AStartIsWantedAgainAfterAStopFromAnotherUnit) {
    F().Build();

    PublishStatus(false, LoggingStatusReason::Unavailable);
    F().control->RequestStart(ALL_UNITS);
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true }));

    zassert_true(F().control->HandleLoggingCommand(false));
    F().control->RequestStart(ALL_UNITS);
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true, true }), "A new request wants a start again");

    PublishStatus(false, LoggingStatusReason::Stopped);
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true, true, true }), "and it is re-asserted again");
}

// ======== Targets: a request names the unit it is for ========

namespace {

constexpr uint8_t LOGGER_ID = 5;
constexpr uint32_t LOGGER_UID = 0x55555555;
constexpr uint8_t OTHER_LOGGER_ID = 6;
constexpr uint32_t OTHER_LOGGER_UID = 0x66666666;

} // namespace

ZTEST(remote_logging_control, test_AStartForAnOnlineLoggerIsSentToItsDeviceId) {
    F().BuildWithNetwork();
    F().network->Add(LOGGER_ID, LOGGER_UID);

    F().control->RequestStart(LOGGER_UID);
    zassert_true(F().sender->WaitForSends(1), "The start is sent");
    F().control->RequestStop(LOGGER_UID);
    zassert_true(F().sender->WaitForSends(2));

    zassert_true(F().sender->Starts() == Starts({ true, false }));
    zassert_equal(F().sender->SentTo(LOGGER_ID), 2U, "to the unit's CDMP device ID");
    zassert_equal(F().sender->Broadcasts(), 0U, "and not as a broadcast");
}

ZTEST(remote_logging_control, test_ARequestForAllUnitsIsOneBroadcast) {
    F().BuildWithNetwork();
    F().network->Add(LOGGER_ID, LOGGER_UID);

    F().control->RequestStart(ALL_UNITS);
    zassert_true(F().sender->WaitForSends(1));

    zassert_equal(F().sender->Broadcasts(), 1U, "One message for all units, whatever the table holds");
    zassert_equal(F().sender->Sends().size(), 1U);
}

ZTEST(remote_logging_control, test_AStartForAnOfflineTargetIsHeldAndSentWhenItRestarts) {
    F().BuildWithNetwork();

    F().control->RequestStart(LOGGER_UID);
    F().Flush();
    zassert_equal(F().sender->Sends().size(), 0U, "The unit is not in the table: the start is held");

    // The unit comes online, and the monitor reports it.
    F().network->Add(LOGGER_ID, LOGGER_UID);
    F().monitor->Report(LOGGER_ID);
    F().Flush();

    zassert_true(F().sender->Starts() == Starts({ true }), "The held start is delivered when the unit appears");
    zassert_equal(F().sender->SentTo(LOGGER_ID), 1U);
}

ZTEST(remote_logging_control, test_AHeldStartIsSentWhenTheStatusReturns) {
    F().BuildWithNetwork();

    PublishStatus(false, LoggingStatusReason::Unavailable);
    F().control->RequestStart(LOGGER_UID);
    F().Flush();
    zassert_equal(F().sender->Sends().size(), 0U, "No unit with that UID is online");

    F().network->Add(LOGGER_ID, LOGGER_UID);
    PublishStatus(false, LoggingStatusReason::Stopped);
    F().Flush();

    zassert_true(F().sender->Starts() == Starts({ true }), "A logging unit appeared: the start goes to the target");
    zassert_equal(F().sender->SentTo(LOGGER_ID), 1U);
}

ZTEST(remote_logging_control, test_AUnitThatCannotLogIsTreatedAsOffline) {
    F().BuildWithNetwork();
    F().network->Add(LOGGER_ID, LOGGER_UID, eerie_leap::tests::DISPLAY_FEATURES);

    F().control->RequestStart(LOGGER_UID);
    F().Flush();
    zassert_equal(F().sender->Sends().size(), 0U, "A display takes no LOGGING start");

    // And one that is offline.
    F().network->Add(
        OTHER_LOGGER_ID, OTHER_LOGGER_UID, eerie_leap::tests::LOGGER_FEATURES,
        FakeNetworkInfo::CdmpDeviceStatus::OFFLINE
    );
    F().control->RequestStart(OTHER_LOGGER_UID);
    F().Flush();
    zassert_equal(F().sender->Sends().size(), 0U, "and neither does an offline logger");
}

ZTEST(remote_logging_control, test_AStopForAnOfflineTargetIsNotHeld) {
    F().BuildWithNetwork();

    F().control->RequestStop(LOGGER_UID);
    F().Flush();

    F().network->Add(LOGGER_ID, LOGGER_UID);
    F().monitor->Report(LOGGER_ID);
    F().Flush();

    zassert_equal(F().sender->Sends().size(), 0U, "No signal delivers a stop later: the unit is not wanted to log");
}

ZTEST(remote_logging_control, test_ARequestForAllUnitsReplacesEveryTargetsWantedState) {
    F().BuildWithNetwork();
    F().network->Add(LOGGER_ID, LOGGER_UID);
    F().network->Add(OTHER_LOGGER_ID, OTHER_LOGGER_UID);

    F().control->RequestStart(LOGGER_UID);
    F().control->RequestStart(OTHER_LOGGER_UID);
    F().Flush();
    zassert_equal(F().sender->Sends().size(), 2U);

    F().control->RequestStop(ALL_UNITS);
    F().Flush();
    zassert_equal(F().sender->Broadcasts(), 1U, "The stop for all units is one broadcast");

    F().monitor->Report(LOGGER_ID);
    F().monitor->Report(OTHER_LOGGER_ID);
    F().Flush();
    zassert_equal(F().sender->Sends().size(), 3U, "and no target wants a start any more");
}

ZTEST(remote_logging_control, test_TargetsAreWantedIndependently) {
    F().BuildWithNetwork();
    F().network->Add(LOGGER_ID, LOGGER_UID);
    F().network->Add(OTHER_LOGGER_ID, OTHER_LOGGER_UID);

    F().control->RequestStart(LOGGER_UID);
    F().control->RequestStart(OTHER_LOGGER_UID);
    F().Flush();
    F().control->RequestStop(LOGGER_UID);
    F().Flush();
    zassert_equal(F().sender->Sends().size(), 3U);

    F().monitor->Report(LOGGER_ID);
    F().Flush();
    zassert_equal(F().sender->Sends().size(), 3U, "The unit that was stopped wants no start");

    F().monitor->Report(OTHER_LOGGER_ID);
    F().Flush();
    zassert_equal(F().sender->Sends().size(), 4U, "The other still does");
    zassert_equal(F().sender->SentTo(OTHER_LOGGER_ID), 2U);
}

ZTEST(remote_logging_control, test_ARestartSendsOneStartToThatUnitOnly) {
    F().BuildWithNetwork();
    F().network->Add(LOGGER_ID, LOGGER_UID);
    F().network->Add(OTHER_LOGGER_ID, OTHER_LOGGER_UID);

    // Every unit wants a start, and this one in particular.
    F().control->RequestStart(ALL_UNITS);
    F().control->RequestStart(LOGGER_UID);
    F().Flush();
    zassert_equal(F().sender->Broadcasts(), 1U);
    zassert_equal(F().sender->SentTo(LOGGER_ID), 1U);

    F().monitor->Report(LOGGER_ID);
    F().Flush();

    zassert_equal(F().sender->Broadcasts(), 1U, "A restart is no broadcast");
    zassert_equal(F().sender->SentTo(LOGGER_ID), 2U, "It is one start to the unit that came back");
    zassert_equal(F().sender->SentTo(OTHER_LOGGER_ID), 0U);
}

ZTEST(remote_logging_control, test_AReceivedStopDropsEveryWantedStart) {
    F().BuildWithNetwork();

    PublishStatus(false, LoggingStatusReason::Unavailable);
    F().control->RequestStart(LOGGER_UID);
    F().control->RequestStart(ALL_UNITS);
    F().control->RequestStart(OTHER_LOGGER_UID);
    F().Flush();
    const size_t sent = F().sender->Sends().size();

    zassert_true(F().control->HandleLoggingCommand(false));

    F().network->Add(LOGGER_ID, LOGGER_UID);
    F().monitor->Report(LOGGER_ID);
    PublishStatus(false, LoggingStatusReason::Stopped);
    F().Flush();

    zassert_equal(F().sender->Sends().size(), sent, "Nothing is re-asserted after the stop");
}

ZTEST(remote_logging_control, test_ATargetThatIsThisUnitSendsNothing) {
    F().BuildWithNetwork();

    F().control->RequestStart(FakeNetworkInfo::OWN_UID);
    F().control->RequestStop(FakeNetworkInfo::OWN_UID);
    F().Flush();

    zassert_equal(F().sender->Sends().size(), 0U, "This unit has no writer: the request is dropped");
}

ZTEST(remote_logging_control, test_ARequestWithoutANetworkInfoIsHeldForever) {
    F().Build();

    F().control->RequestStart(LOGGER_UID);
    F().monitor->Report(LOGGER_ID);
    F().Flush();

    zassert_equal(F().sender->Sends().size(), 0U, "A UID never resolves without the device table");
}

ZTEST(remote_logging_control, test_PeerModeIgnoresTheStatus) {
    F().BuildWithNetwork(RemoteLoggingControl::Mode::Peer);

    PublishStatus(true);
    zassert_false(F().control->IsActive(), "A peer follows no status");

    PublishStatus(false, LoggingStatusReason::Unavailable);
    F().control->RequestStart(ALL_UNITS);
    F().Flush();
    PublishStatus(false, LoggingStatusReason::Stopped);
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true }), "and has no status rule: the start is not sent again");

    // The restart monitor still works for it.
    F().monitor->Report(LOGGER_ID);
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({ true, true }));
    zassert_equal(F().sender->SentTo(LOGGER_ID), 1U);
}

// A stop for one unit, after a start for all units, is the latest request for that unit.
ZTEST(remote_logging_control, test_AStopForOneUnitAfterAStartForAllIsNotUndoneByARestart) {
    F().BuildWithNetwork();
    F().network->Add(LOGGER_ID, LOGGER_UID);
    F().network->Add(OTHER_LOGGER_ID, OTHER_LOGGER_UID);

    F().control->RequestStart(ALL_UNITS);
    F().Flush();
    F().control->RequestStop(LOGGER_UID);
    F().Flush();
    zassert_equal(F().sender->Broadcasts(), 1U);
    zassert_equal(F().sender->SentTo(LOGGER_ID), 1U, "The stop goes to the unit");

    F().monitor->Report(LOGGER_ID);
    F().Flush();
    zassert_equal(F().sender->SentTo(LOGGER_ID), 1U, "The unit that was stopped is not started by its restart");

    F().monitor->Report(OTHER_LOGGER_ID);
    F().Flush();
    zassert_equal(F().sender->SentTo(OTHER_LOGGER_ID), 1U, "The other unit still is");
}

ZTEST(remote_logging_control, test_TheStatusRuleLeavesOutAUnitThatWasStopped) {
    F().BuildWithNetwork();
    F().network->Add(LOGGER_ID, LOGGER_UID);
    F().network->Add(OTHER_LOGGER_ID, OTHER_LOGGER_UID);

    PublishStatus(false, LoggingStatusReason::Unavailable);
    F().control->RequestStart(ALL_UNITS);
    F().Flush();
    F().control->RequestStop(LOGGER_UID);
    F().Flush();
    zassert_equal(F().sender->Broadcasts(), 1U);

    PublishStatus(false, LoggingStatusReason::Stopped);
    F().Flush();

    zassert_equal(F().sender->Broadcasts(), 1U, "No second broadcast: it would start the stopped unit");
    zassert_equal(F().sender->SentTo(LOGGER_ID), 1U, "The stopped unit gets nothing more");
    zassert_equal(F().sender->SentTo(OTHER_LOGGER_ID), 1U, "The other gets a start of its own");
}

// A unit whose features gain LOGGING after the status or the restart was handled: the held start goes
// out on the next report.
ZTEST(remote_logging_control, test_AHeldStartIsSentOnceTheUnitGainsTheLoggingFeature) {
    F().BuildWithNetwork();
    F().network->Add(LOGGER_ID, LOGGER_UID, eerie_leap::tests::DISPLAY_FEATURES);

    PublishStatus(false, LoggingStatusReason::Unavailable);
    F().control->RequestStart(LOGGER_UID);
    PublishStatus(false, LoggingStatusReason::Stopped);
    F().Flush();
    zassert_equal(F().sender->Sends().size(), 0U, "The unit cannot log yet: the start is held");

    F().network->Add(LOGGER_ID, LOGGER_UID);
    F().monitor->Report(LOGGER_ID);
    F().Flush();

    zassert_true(F().sender->Starts() == Starts({ true }), "The unit gained the feature: the held start goes out");
    zassert_equal(F().sender->SentTo(LOGGER_ID), 1U);
}
