#include <atomic>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

#include <zephyr/ztest.h>

#include "domain/canbus_com_domain/commands/canbus_com_command_code.h"
#include "domain/canbus_com_domain/commands/canbus_com_logging_command.h"
#include "domain/logging_domain/control/logging_command_handler.h"
#include "domain/logging_domain/control/remote_logging_control.h"

#include "canbus_com_loopback_harness.h"
#include "logging_control_test_support.h"

using namespace logging_control_test;
using namespace canbus_com_loopback_test;

using eerie_leap::domain::canbus_com_domain::commands::CanbusComCommandCode;
using eerie_leap::domain::canbus_com_domain::commands::CanbusComLoggingCommand;
using eerie_leap::domain::logging_domain::control::LoggingCommandHandler;
using eerie_leap::domain::logging_domain::control::RemoteLoggingControl;

namespace {

constexpr uint8_t LOGGING_CODE = std::to_underlying(CanbusComCommandCode::LOGGING);

// The unit, a scripted peer on its bus, and the handler under test with a fake control.
struct Fixture {
    Harness harness;
    std::shared_ptr<FakeLoggingControl> control = std::make_shared<FakeLoggingControl>();
    std::shared_ptr<LoggingCommandHandler> handler;
    uint8_t next_transaction = 1;

    // A real remote control for the cases of the role without a writer; built by BuildRemote().
    std::shared_ptr<FakeCommandSender> remote_sender = std::make_shared<FakeCommandSender>();
    std::shared_ptr<FakeRestartMonitor> remote_monitor = std::make_shared<FakeRestartMonitor>();
    std::shared_ptr<RemoteLoggingControl> remote;

    Fixture() {
        harness.BringOnline();
        harness.Settle();
    }

    void BuildHandler() {
        handler = std::make_shared<LoggingCommandHandler>(harness.com, control);
    }

    // The role of the gauge: the handler over a RemoteLoggingControl.
    void BuildRemote() {
        remote = std::make_shared<RemoteLoggingControl>(TestConfigQueue(), remote_sender, remote_monitor);
        zassert_true(remote->Initialize(), "The remote control did not initialize.");
        handler = std::make_shared<LoggingCommandHandler>(harness.com, remote);
    }

    // A LOGGING request from the peer to the unit; the transaction it carries.
    uint8_t SendRequest(const Bytes& data) {
        const uint8_t transaction = next_transaction++;

        harness.SendAsPeer(COMMAND, CdmpCommandRequestMessage{
            .target_device_id = OWN_ID,
            .command_code = LOGGING_CODE,
            .transaction_id = transaction,
            .data = data}.ToCanFrame());

        return transaction;
    }

    // The unit's answer to a transaction, or nothing in time.
    std::optional<CdmpCommandResponseMessage> WaitForResponse(uint8_t transaction, int timeout_ms = FRAME_TIMEOUT_MS) {
        auto frame = harness.bus.Wait(
            COMMAND_RESPONSE,
            [transaction](const Bytes& bytes) {
                return bytes.size() >= 4 && bytes[0] == OWN_ID && bytes[1] == LOGGING_CODE && bytes[2] == transaction;
            },
            timeout_ms);
        if(!frame.has_value())
            return std::nullopt;

        return CdmpCommandResponseMessage::FromCanFrame(frame->data);
    }

    std::optional<CdmpResultCode> Ask(const Bytes& data) {
        const auto response = WaitForResponse(SendRequest(data));
        if(!response.has_value())
            return std::nullopt;

        return response->result_code;
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

    // The handler before the harness, so it is gone when the COM service stops.
    if(Current() != nullptr) {
        Current()->handler.reset();
        Current()->remote.reset();
        delete Current();
        Current() = nullptr;
    }

    ResetLoopbackDevice();
}

Fixture& F() {
    return *Current();
}

} // namespace

ZTEST_SUITE(logging_command_handler, NULL, SetUp, Before, After, NULL);

ZTEST(logging_command_handler, test_AStartIsAnsweredBeforeItRuns) {
    const auto queue = TestConfigQueue();
    F().control->RunJobOn(queue);
    F().BuildHandler();

    // The work a start does queues up behind the gate; the answer must not wait for it.
    zassert_true(Gate().Close(*queue), "The queue must be held");

    // The queue stays held, so only the handler can make the answer late: one that waited for the
    // outcome (the fake never reports one) would not answer within this.
    const int64_t start_ms = k_uptime_get();
    const auto result = F().Ask({0x01});
    const int64_t answer_ms = k_uptime_get() - start_ms;
    zassert_true(result.has_value(), "The start must be answered");
    zassert_equal(*result, CdmpResultCode::SUCCESS, "which means accepted");
    zassert_true(answer_ms < k_no_wait_ms, "The answer must not wait for the start (%lld ms)",
        static_cast<long long>(answer_ms));
    zassert_false(F().control->WaitForJob(k_no_wait_ms), "The start had not run when it was answered");

    const auto requests = F().control->Requests();
    zassert_equal(requests.size(), 1);
    zassert_equal(requests[0].kind, Kind::Start);
    zassert_equal(requests[0].thread_name, CDMP_WORK_QUEUE_NAME, "The request is made on the CDMP work queue");

    Gate().Open();
    zassert_true(F().control->WaitForJob(), "The start runs once the queue is free");
}

ZTEST(logging_command_handler, test_AStopIsAccepted) {
    F().BuildHandler();

    const auto result = F().Ask({0x00});
    zassert_true(result.has_value());
    zassert_equal(*result, CdmpResultCode::SUCCESS);

    zassert_true(F().control->Kinds() == Kinds({Kind::Stop}));
}

ZTEST(logging_command_handler, test_ARequestWithoutAPayloadFails) {
    F().BuildHandler();

    const auto result = F().Ask({});
    zassert_true(result.has_value());
    zassert_equal(*result, CdmpResultCode::FAILURE, "A request that names neither start nor stop fails");

    zassert_equal(F().control->Requests().size(), 0);
}

ZTEST(logging_command_handler, test_DestroyingTheHandlerUnregistersTheCommand) {
    F().BuildHandler();
    zassert_true(F().Ask({0x01}).has_value());
    zassert_equal(F().control->Requests().size(), 1);

    F().handler.reset();

    const auto result = F().Ask({0x01});
    zassert_true(result.has_value());
    zassert_equal(*result, CdmpResultCode::UNSUPPORTED_COMMAND, "No handler, no command");
    zassert_equal(F().control->Requests().size(), 1, "and no request");
}

ZTEST(logging_command_handler, test_DestroyingTheHandlerWaitsForACallInProgress) {
    F().control->HoldRequests();
    F().BuildHandler();

    const uint8_t transaction = F().SendRequest({0x01});
    zassert_true(F().control->WaitUntilHeld(), "The request must reach the control");

    // Destroyed on another thread, which has to wait for the call on the CDMP work queue.
    struct Destruction {
        k_sem done;
        std::shared_ptr<LoggingCommandHandler> handler;
    };
    auto destruction = std::make_shared<Destruction>();
    k_sem_init(&destruction->done, 0, 1);
    destruction->handler = std::move(F().handler);

    WorkQueueThread helper("handler_destroyer", 4096, 5);
    zassert_true(helper.Initialize());
    helper.Run([destruction] {
        destruction->handler.reset();
        k_sem_give(&destruction->done);
    });

    zassert_not_equal(k_sem_take(&destruction->done, Ms(NO_FRAME_TIMEOUT_MS)), 0,
        "The destructor must wait for the call in progress");

    F().control->Release();

    const auto response = F().WaitForResponse(transaction);
    zassert_true(response.has_value(), "The call in progress is answered");
    zassert_equal(response->result_code, CdmpResultCode::SUCCESS, "as it was accepted");
    zassert_equal(k_sem_take(&destruction->done, Ms(k_wait_ms)), 0, "and the destructor returns");

    const auto later = F().Ask({0x01});
    zassert_true(later.has_value());
    zassert_equal(*later, CdmpResultCode::UNSUPPORTED_COMMAND);
    zassert_equal(F().control->Requests().size(), 1, "One request in all");
}

ZTEST(logging_command_handler, test_ANullArgumentIsRefused) {
    bool is_service_refused = false;
    try {
        LoggingCommandHandler handler(nullptr, F().control);
    } catch(const std::invalid_argument&) {
        is_service_refused = true;
    }
    zassert_true(is_service_refused, "A handler needs the COM service");

    bool is_control_refused = false;
    try {
        LoggingCommandHandler handler(F().harness.com, nullptr);
    } catch(const std::invalid_argument&) {
        is_control_refused = true;
    }
    zassert_true(is_control_refused, "and a control");

    // The refusals registered nothing.
    const auto result = F().Ask({0x01});
    zassert_true(result.has_value());
    zassert_equal(*result, CdmpResultCode::UNSUPPORTED_COMMAND);
}

// The slot CDMP keeps the handler in, which LoggingCommandHandler's destructor unsets while the CDMP
// work queue may be in a call: a call takes its own reference to the handler, and the lock is not
// held while the handler runs. These cases use the COM service's raw registration.
namespace {

// A handler that blocks inside its call and owns a sentinel, so a test can see how long the handler
// lives. The test keeps the semaphores through the shared signals, never the closure.
struct Signals {
    k_sem entered;
    k_sem release;
    std::atomic<int> calls = 0;

    Signals() {
        k_sem_init(&entered, 0, K_SEM_MAX_LIMIT);
        k_sem_init(&release, 0, K_SEM_MAX_LIMIT);
    }
};

std::weak_ptr<int> RegisterBlockingHandler(Fixture& fixture, const std::shared_ptr<Signals>& signals, bool result) {
    auto sentinel = std::make_shared<int>(0);
    std::weak_ptr<int> weak_sentinel = sentinel;

    fixture.harness.com->SetCommandHandler<CanbusComLoggingCommand>(
        CanbusComCommandCode::LOGGING,
        [signals, sentinel = std::move(sentinel), result](std::optional<CanbusComLoggingCommand>) {
            signals->calls++;
            k_sem_give(&signals->entered);
            // Gives up after 5 s, so a case that never releases cannot wedge the CDMP queue for good.
            k_sem_take(&signals->release, K_SECONDS(5));

            return result;
        });

    return weak_sentinel;
}

} // namespace

ZTEST(logging_command_handler, test_AHandlerUnsetDuringItsCallFinishesThatCall) {
    auto signals = std::make_shared<Signals>();
    const auto weak_sentinel = RegisterBlockingHandler(F(), signals, true);
    zassert_false(weak_sentinel.expired(), "the registered handler holds the sentinel");

    const uint8_t transaction = F().SendRequest({0x01});
    zassert_equal(k_sem_take(&signals->entered, Ms(k_wait_ms)), 0, "The handler must be called");

    // The CDMP work queue is inside the call: unsetting neither waits for it nor ends it.
    const int64_t start_ms = k_uptime_get();
    F().harness.com->UnsetCommandHandler(CanbusComCommandCode::LOGGING);
    const int64_t elapsed_ms = k_uptime_get() - start_ms;
    zassert_true(elapsed_ms < k_no_wait_ms, "Unsetting the handler waited %lld ms for its call", static_cast<long long>(elapsed_ms));
    zassert_false(weak_sentinel.expired(), "The call in progress must keep its handler alive");

    k_sem_give(&signals->release);
    const auto response = F().WaitForResponse(transaction);
    zassert_true(response.has_value(), "The call in progress is answered");
    zassert_equal(response->result_code, CdmpResultCode::SUCCESS, "by the handler it started on");

    // Released when the call returned, before the answer went out.
    zassert_true(weak_sentinel.expired(), "The handler must go once its call returned");

    const auto later = F().Ask({0x01});
    zassert_true(later.has_value());
    zassert_equal(*later, CdmpResultCode::UNSUPPORTED_COMMAND, "A handler that was unset is not called again");
    zassert_equal(signals->calls.load(), 1);
}

ZTEST(logging_command_handler, test_AHandlerReplacedDuringItsCallFinishesThatCall) {
    auto signals = std::make_shared<Signals>();
    const auto weak_sentinel = RegisterBlockingHandler(F(), signals, true);

    const uint8_t transaction = F().SendRequest({0x01});
    zassert_equal(k_sem_take(&signals->entered, Ms(k_wait_ms)), 0, "The handler must be called");

    // The second handler answers FAILURE at once.
    auto replacement_calls = std::make_shared<std::atomic<int>>(0);
    const int64_t start_ms = k_uptime_get();
    F().harness.com->SetCommandHandler<CanbusComLoggingCommand>(
        CanbusComCommandCode::LOGGING,
        [replacement_calls](std::optional<CanbusComLoggingCommand>) {
            (*replacement_calls)++;

            return false;
        });
    zassert_true(k_uptime_get() - start_ms < k_no_wait_ms, "Replacing the handler must not wait for its call");
    zassert_false(weak_sentinel.expired(), "The call in progress must keep the old handler alive");

    k_sem_give(&signals->release);
    const auto response = F().WaitForResponse(transaction);
    zassert_true(response.has_value());
    zassert_equal(response->result_code, CdmpResultCode::SUCCESS, "The call in progress finishes on the old handler");
    zassert_true(weak_sentinel.expired());

    const auto later = F().Ask({0x01});
    zassert_true(later.has_value());
    zassert_equal(*later, CdmpResultCode::FAILURE, "and the next call is the replacement's");
    zassert_equal(replacement_calls->load(), 1);
    zassert_equal(signals->calls.load(), 1);
}

// The gauge answers the command too (L9): a stop is accepted and drops its wanted start, so no
// re-assertion sends a start that undoes it.
ZTEST(logging_command_handler, test_TheRemoteRoleAnswersAStopAndDropsItsWantedStart) {
    F().BuildRemote();

    PublishStatus(false, LoggingStatusReason::Unavailable);
    F().remote->RequestStart();
    zassert_true(F().remote_sender->WaitForSends(1), "The start is sent");

    const auto result = F().Ask({0x00});
    zassert_true(result.has_value(), "A unit without a writer answers a stop");
    zassert_equal(*result, CdmpResultCode::SUCCESS);

    PublishStatus(false, LoggingStatusReason::Stopped);
    F().remote_monitor->Report(PEER_ID);
    zassert_true(FlushQueue(*TestConfigQueue()));
    zassert_equal(F().remote_sender->Sends().size(), 1U, "and sends no start again, whatever reports a logging unit");
}

ZTEST(logging_command_handler, test_TheRemoteRoleAnswersAStartFailure) {
    F().BuildRemote();

    const auto result = F().Ask({0x01});
    zassert_true(result.has_value(), "It answers a start too");
    zassert_equal(*result, CdmpResultCode::FAILURE, "but cannot serve it");

    zassert_true(FlushQueue(*TestConfigQueue()));
    zassert_equal(F().remote_sender->Sends().size(), 0U, "and sends nothing");
}
