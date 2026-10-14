#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "subsys/threading/work_queue_thread.h"
#include "subsys/cdmp/models/messages/cdmp_command_response_message.h"
#include "subsys/cdmp/services/command_service/cdmp_transaction_service.h"

using namespace eerie_leap::subsys::cdmp::models;
using namespace eerie_leap::subsys::cdmp::services::command_service;
using namespace eerie_leap::subsys::cdmp::utilities;

using eerie_leap::subsys::threading::WorkQueueThread;

namespace {

constexpr uint8_t TARGET_ID = 4;
constexpr uint8_t OTHER_ID = 5;
constexpr uint8_t LOGGING_COMMAND = 0x20;
constexpr uint8_t STATE_SYNC_REQUEST = std::to_underlying(CdmpServiceCommandCode::STATE_SYNC_REQUEST);

struct TransactionFixture {
    std::shared_ptr<WorkQueueThread> work_queue = std::make_shared<WorkQueueThread>(
        "cdmp_test_wq",
        CONFIG_EERIE_LEAP_CDMP_WORK_QUEUE_STACK_SIZE,
        CONFIG_EERIE_LEAP_CDMP_WORK_QUEUE_PRIORITY);
    std::shared_ptr<CdmpDevice> device = std::make_shared<CdmpDevice>(0x11111111, CdmpDeviceType::DISPLAY);
    std::unique_ptr<CdmpTransactionService> transactions;

    k_sem completed{};
    std::vector<CdmpResultCode> results;

    TransactionFixture() {
        zassert_true(work_queue->Initialize());
        transactions = std::make_unique<CdmpTransactionService>(work_queue, device);
        transactions->Initialize();
        k_sem_init(&completed, 0, K_SEM_MAX_LIMIT);
    }

    ~TransactionFixture() {
        transactions.reset();
        work_queue->Stop();
    }

    uint8_t Start(uint8_t target_device_id, uint8_t command_code, int timeout_ms = 1000) {
        return transactions->StartTransaction(target_device_id, command_code, timeout_ms,
            [this](uint8_t, CdmpResultCode result_code, std::span<const uint8_t>) {
                results.push_back(result_code);
                k_sem_give(&completed);
            });
    }

    void Respond(uint8_t source, uint8_t command_code, uint8_t transaction_id) {
        transactions->CompleteTransaction({
            .source_device_id = source,
            .command_code = command_code,
            .transaction_id = transaction_id,
            .result_code = CdmpResultCode::SUCCESS,
            .data = {}});
    }
};

} // namespace

ZTEST_SUITE(cdmp_transaction, NULL, NULL, NULL, NULL, NULL);

ZTEST(cdmp_transaction, test_response_must_echo_the_command_and_come_from_the_target) {
    TransactionFixture fixture;
    const uint8_t transaction_id = fixture.Start(TARGET_ID, LOGGING_COMMAND);

    // Other exchanges on the bus that happen to carry the same transaction ID.
    fixture.Respond(TARGET_ID, STATE_SYNC_REQUEST, transaction_id);
    fixture.Respond(OTHER_ID, LOGGING_COMMAND, transaction_id);

    zassert_true(fixture.transactions->IsTransactionPending(transaction_id));
    zassert_true(fixture.results.empty(), "Neither answers this request");

    fixture.Respond(TARGET_ID, LOGGING_COMMAND, transaction_id);

    zassert_false(fixture.transactions->IsTransactionPending(transaction_id));
    zassert_equal(fixture.results.size(), 1);
    zassert_equal(fixture.results[0], CdmpResultCode::SUCCESS);
}

ZTEST(cdmp_transaction, test_any_device_answers_a_broadcast_request) {
    TransactionFixture fixture;
    const uint8_t transaction_id = fixture.Start(CdmpDevice::DEVICE_ID_BROADCAST, LOGGING_COMMAND);

    fixture.Respond(OTHER_ID, LOGGING_COMMAND, transaction_id);

    zassert_false(fixture.transactions->IsTransactionPending(transaction_id));
    zassert_equal(fixture.results.size(), 1);
}

ZTEST(cdmp_transaction, test_unanswered_transaction_times_out) {
    TransactionFixture fixture;
    fixture.transactions->Start();

    const uint8_t transaction_id = fixture.Start(TARGET_ID, LOGGING_COMMAND, 50);

    zassert_equal(k_sem_take(&fixture.completed, K_MSEC(2 * CONFIG_EERIE_LEAP_CDMP_CMD_TRANSACTION_TIMEOUT_MS)), 0,
        "The timeout task expires it");
    zassert_equal(fixture.results[0], CdmpResultCode::TIMEOUT);
    zassert_false(fixture.transactions->IsTransactionPending(transaction_id));
}

ZTEST(cdmp_transaction, test_stop_cancels_pending_transactions) {
    TransactionFixture fixture;
    fixture.transactions->Start();

    fixture.Start(TARGET_ID, LOGGING_COMMAND);
    fixture.Start(CdmpDevice::DEVICE_ID_BROADCAST, STATE_SYNC_REQUEST);
    zassert_equal(fixture.transactions->GetPendingTransactionCount(), 2);

    fixture.transactions->Stop();

    zassert_equal(fixture.transactions->GetPendingTransactionCount(), 0);
    zassert_equal(fixture.results.size(), 2);
    for(auto result_code : fixture.results)
        zassert_equal(result_code, CdmpResultCode::CANCELLED);

    // Stopped, the timeout task no longer runs, so a new transaction stays pending.
    const uint8_t transaction_id = fixture.Start(TARGET_ID, LOGGING_COMMAND, 10);
    k_msleep(CONFIG_EERIE_LEAP_CDMP_CMD_TRANSACTION_TIMEOUT_MS);
    zassert_true(fixture.transactions->IsTransactionPending(transaction_id));
}
