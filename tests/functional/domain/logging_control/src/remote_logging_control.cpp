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

// The control under test on the binary's configuration queue, with a fake in place of the COM service.
struct Fixture {
    std::shared_ptr<WorkQueueThread> queue = TestConfigQueue();
    std::shared_ptr<FakeCommandSender> sender = std::make_shared<FakeCommandSender>();
    std::unique_ptr<RemoteLoggingControl> control;

    void Build() {
        control = std::make_unique<RemoteLoggingControl>(queue, sender);
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

    F().control->RequestStart();
    zassert_true(F().sender->WaitForSends(1), "A start request must be sent");

    F().control->RequestStop();
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
    F().control->RequestStart();
    F().control->RequestStop();
    F().control->RequestStart();
    zassert_true(k_uptime_get() - start_ms < k_no_wait_ms, "Requests must not wait for the send");

    Gate().Open();
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({true}), "Start, stop, start is one command: the latest");

    zassert_true(Gate().Close(*F().queue));
    F().control->RequestStart();
    F().control->RequestStop();
    Gate().Open();
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({true, false}), "Start, stop is a stop");
}

ZTEST(remote_logging_control, test_AWantedStartIsSentAgainWhenALoggingUnitAppears) {
    F().Build();

    // No logging unit is online when the start is wanted.
    PublishStatus(false, LoggingStatusReason::Unavailable);
    F().control->RequestStart();
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({true}));

    PublishStatus(false, LoggingStatusReason::Stopped);
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({true, true}), "A logging unit appeared: the start is sent again");

    PublishStatus(false, LoggingStatusReason::Stopped);
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({true, true}), "Stopped, then Stopped again, is no appearance");

    // The unit goes away and comes back: once more.
    PublishStatus(false, LoggingStatusReason::Unavailable);
    PublishStatus(false, LoggingStatusReason::Stopped);
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({true, true, true}), "Each appearance is one re-assertion");
}

// A request and a logging unit's appearance taken in one run: the request's send is the re-assertion.
ZTEST(remote_logging_control, test_AReassertionInTheRunOfARequestIsThatRequest) {
    F().Build();

    PublishStatus(false, LoggingStatusReason::Unavailable);

    zassert_true(Gate().Close(*F().queue), "The queue must be held");
    F().control->RequestStart();
    PublishStatus(false, LoggingStatusReason::Stopped);
    Gate().Open();
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({true}), "One start for the request and the appearance");

    // The unit goes away, and a stop is requested in the run that sees it come back.
    PublishStatus(false, LoggingStatusReason::Unavailable);
    zassert_true(Gate().Close(*F().queue), "The queue must be held");
    PublishStatus(false, LoggingStatusReason::Stopped);
    F().control->RequestStop();
    Gate().Open();
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({true, false}), "The stop is sent and no start follows it");
}

ZTEST(remote_logging_control, test_NoStartIsSentAgainAfterLoggingRan) {
    F().Build();

    PublishStatus(false, LoggingStatusReason::Unavailable);
    F().control->RequestStart();
    F().Flush();

    PublishStatus(true);
    PublishStatus(false, LoggingStatusReason::Stopped);
    F().Flush();

    zassert_true(F().sender->Starts() == Starts({true}), "A unit that logged and stopped stands as it stopped");
}

ZTEST(remote_logging_control, test_NoStartIsSentAgainAfterAStop) {
    F().Build();

    F().control->RequestStart();
    F().Flush();
    F().control->RequestStop();
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({true, false}));

    PublishStatus(false, LoggingStatusReason::Unavailable);
    PublishStatus(false, LoggingStatusReason::Stopped);
    F().Flush();

    zassert_true(F().sender->Starts() == Starts({true, false}), "A stop ends what was wanted");
}

ZTEST(remote_logging_control, test_NoStartIsSentAgainAfterAFailure) {
    F().Build();

    F().control->RequestStart();
    F().Flush();

    PublishStatus(false, LoggingStatusReason::Unavailable);
    PublishStatus(false, LoggingStatusReason::NoSpace);
    PublishStatus(false, LoggingStatusReason::Stopped);
    F().Flush();

    zassert_true(F().sender->Starts() == Starts({true}), "A failure stands until the switch moves");
}

ZTEST(remote_logging_control, test_TheFirstStatusSendsNothing) {
    F().Build();

    F().control->RequestStart();
    F().Flush();

    // No predecessor: the unit was there, or this is the first we hear of it, and the start was sent.
    PublishStatus(false, LoggingStatusReason::Stopped);
    F().Flush();

    zassert_true(F().sender->Starts() == Starts({true}));
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
    F().control->RequestStart();
    F().Flush();

    // The fence events of the blockers carry no IsActive.
    LoggingEventsChannel::GetInstance().Publish({.source_id = k_fence_source_id, .type = LoggingEventType::StatusUpdated});
    PublishStatus(false, LoggingStatusReason::Stopped);
    F().Flush();

    zassert_true(F().sender->Starts() == Starts({true, true}), "An event without IsActive does not reset the predecessor");
}

ZTEST(remote_logging_control, test_IsActiveFollowsTheStatus) {
    F().Build();

    zassert_false(F().control->IsActive(), "Nothing logs before a status");

    PublishStatus(true);
    zassert_true(F().control->IsActive());

    PublishStatus(false, LoggingStatusReason::Stopped);
    zassert_false(F().control->IsActive());
}

ZTEST(remote_logging_control, test_ARequestBeforeInitializeSendsNothing) {
    F().control = std::make_unique<RemoteLoggingControl>(F().queue, F().sender);

    F().control->RequestStart();
    F().control->RequestStop();
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

    control->RequestStart();
    control->RequestStop();
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
    F().control->RequestStart();
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
    F().control->RequestStart();
    zassert_true(F().sender->WaitUntilHeld(), "The send must be running");

    // Destroyed on another thread, which has to wait for the send on the configuration work queue;
    // the destructor must not run on that queue.
    struct Destruction {
        k_sem done;
        std::unique_ptr<RemoteLoggingControl> control;
    };
    auto destruction = std::make_shared<Destruction>();
    k_sem_init(&destruction->done, 0, 1);
    destruction->control = std::move(F().control);

    WorkQueueThread helper("control_destroyer", 4096, 5);
    zassert_true(helper.Initialize());
    helper.Run([destruction] {
        destruction->control.reset();
        k_sem_give(&destruction->done);
    });

    zassert_not_equal(k_sem_take(&destruction->done, Ms(k_no_wait_ms)), 0,
        "The destructor must wait for the send in progress");

    F().sender->Release();
    zassert_equal(k_sem_take(&destruction->done, Ms(k_wait_ms)), 0, "and return once it finishes");

    F().Flush();
    zassert_true(F().sender->Starts() == Starts({true}), "One send in all");
}

ZTEST(remote_logging_control, test_AStatusInFlightDuringDestructionDoesNothing) {
    // Made before the control, so its handler runs first in a dispatch and holds the bus thread there.
    LoggingBlocker blocker(LoggingEventsChannel::GetInstance(), LoggingEventType::StatusUpdated);

    F().Build();

    PublishStatus(false, LoggingStatusReason::Unavailable);
    F().control->RequestStart();
    F().Flush();
    zassert_true(F().sender->Starts() == Starts({true}));

    blocker.Arm();
    PublishStatus(false, LoggingStatusReason::Stopped, true);
    zassert_true(blocker.WaitUntilBlocking(), "The bus thread is inside the dispatch");

    // The bus has the control's handler in its snapshot of this dispatch, and the status would
    // re-assert the start.
    F().control.reset();

    blocker.Release();
    zassert_true(blocker.Fence(), "The dispatch finished");
    F().Flush();

    zassert_true(F().sender->Starts() == Starts({true}), "A detached handler sends nothing");
}
