#include <zephyr/logging/log.h>
#include <zephyr/sys/timeutil.h>

#include "cdmp_transaction_service.h"

namespace eerie_leap::subsys::cdmp::services::command_service {

LOG_MODULE_REGISTER(cdmp_transaction_service, LOG_LEVEL_INF);

CdmpTransactionService::CdmpTransactionService(
    std::shared_ptr<WorkQueueThread> work_queue_thread,
    std::shared_ptr<CdmpDevice> device)
        : work_queue_thread_(std::move(work_queue_thread))
        , device_(std::move(device)) {}

void CdmpTransactionService::Initialize() {
    timeout_task_ = work_queue_thread_->CreateTask(
        ProcessTransactionTimeouts, this);
}

CdmpTransactionService::~CdmpTransactionService() {
    Stop();
}

void CdmpTransactionService::Start() {
    if(!is_timeout_task_running_) {
        timeout_task_.value().Schedule();
        is_timeout_task_running_ = true;

        LOG_INF("CDMP Transaction Service started");
    }
}

void CdmpTransactionService::Stop() {
    // Cancel all pending transactions
    for(auto& [transaction_id, _] : pending_transactions_)
        CancelTransaction(transaction_id);

    is_timeout_task_running_ = false;
    if(timeout_task_.has_value())
        timeout_task_.value().Cancel();

    LOG_INF("CDMP Transaction Service stopped");
}

uint8_t CdmpTransactionService::GetNextTransactionId() {
    uint8_t transaction_id = next_transaction_id_;

    // Skip transaction ID 0 and find next available
    do {
        next_transaction_id_++;
        if(next_transaction_id_ == 0)
            next_transaction_id_ = 1;
    } while(pending_transactions_.find(next_transaction_id_) != pending_transactions_.end()
        && next_transaction_id_ != transaction_id);

    return transaction_id;
}

uint8_t CdmpTransactionService::StartTransaction(
    uint8_t expected_command,
    int timeout,
    CdmpTransactionCallback callback) {

    uint8_t transaction_id = GetNextTransactionId();

    PendingTransaction transaction{
        .transaction_id = transaction_id,
        .start_time = static_cast<uint64_t>(k_uptime_get()),
        .timeout = static_cast<uint64_t>(timeout),
        .callback = std::move(callback),
        .expected_command = expected_command
    };

    pending_transactions_[transaction_id] = std::move(transaction);

    LOG_DBG("Started transaction %d for command %d", transaction_id, expected_command);

    return transaction_id;
}

void CdmpTransactionService::CompleteTransaction(const CdmpCommandResponseMessage& response) {
    auto it = pending_transactions_.find(response.transaction_id);
    if(it != pending_transactions_.end()) {
        if(it->second.callback)
            it->second.callback(response.transaction_id, response.result_code, response.data);

        pending_transactions_.erase(it);

        LOG_DBG("Completed transaction %d", response.transaction_id);
    }
}

void CdmpTransactionService::CancelTransaction(uint8_t transaction_id) {
    auto it = pending_transactions_.find(transaction_id);
    if(it != pending_transactions_.end()) {
        if(it->second.callback)
            it->second.callback(transaction_id, CdmpResultCode::CANCELLED, {});

        pending_transactions_.erase(it);

        LOG_DBG("Cancelled transaction %d", transaction_id);
    }
}

void CdmpTransactionService::CleanupExpiredTransactions() {
    uint64_t current_time = k_uptime_get();

    std::vector<uint8_t> expired_transactions;

    for(auto& [transaction_id, transaction] : pending_transactions_) {
        if(current_time - transaction.start_time >= transaction.timeout)
            expired_transactions.push_back(transaction_id);
    }

    for(auto transaction_id : expired_transactions) {
        auto& transaction = pending_transactions_.at(transaction_id);
        if(transaction.callback)
            transaction.callback(transaction_id, CdmpResultCode::TIMEOUT, {});

        pending_transactions_.erase(transaction_id);

        LOG_DBG("Transaction %d timed out", transaction_id);
    }
}

bool CdmpTransactionService::IsTransactionPending(uint8_t transaction_id) const {
    return pending_transactions_.contains(transaction_id);
}

WorkQueueTaskResult CdmpTransactionService::ProcessTransactionTimeouts(CdmpTransactionService* instance) {
    instance->CleanupExpiredTransactions();

    return {
        .reschedule = instance->is_timeout_task_running_,
        .delay = K_MSEC(CONFIG_EERIE_LEAP_CDMP_CMD_TRANSACTION_TIMEOUT_MS / 2)
    };
}

} // namespace eerie_leap::subsys::cdmp::services::command_service
