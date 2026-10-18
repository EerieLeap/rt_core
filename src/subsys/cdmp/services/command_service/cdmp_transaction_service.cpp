#include <algorithm>

#include <zephyr/logging/log.h>
#include <zephyr/sys/timeutil.h>

#include "subsys/threading/scoped_mutex.h"

#include "cdmp_transaction_service.h"

namespace eerie_leap::subsys::cdmp::services::command_service {

LOG_MODULE_REGISTER(cdmp_transaction_service, LOG_LEVEL_INF);

using eerie_leap::subsys::threading::ScopedMutex;

CdmpTransactionService::CdmpTransactionService(
    std::shared_ptr<WorkQueueThread> work_queue_thread,
    std::shared_ptr<CdmpDevice> device
) :
    work_queue_thread_(std::move(work_queue_thread)),
    device_(std::move(device)) {

    k_mutex_init(&transactions_lock_);
}

void CdmpTransactionService::Initialize() {
    timeout_task_ = work_queue_thread_->CreateTask(ProcessTransactionTimeouts, this);
}

CdmpTransactionService::~CdmpTransactionService() {
    Stop();
}

void CdmpTransactionService::Start() {
    ScopedMutex guard(transactions_lock_);

    if(!is_timeout_task_running_) {
        timeout_task_.value().Schedule();
        is_timeout_task_running_ = true;

        LOG_INF("CDMP Transaction Service started");
    }
}

void CdmpTransactionService::Stop() {
    {
        ScopedMutex guard(transactions_lock_);
        is_timeout_task_running_ = false;
    }

    // A pass that rescheduled itself before the flag was cleared has its timer removed here, and a
    // later pass does not reschedule.
    if(timeout_task_.has_value())
        timeout_task_.value().Cancel();

    Transactions cancelled;
    {
        ScopedMutex guard(transactions_lock_);
        cancelled.swap(pending_transactions_);
    }

    for(auto& [_, transaction] : cancelled)
        InvokeCallback(transaction, CdmpResultCode::CANCELLED, {});

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
    uint8_t target_device_id,
    uint8_t expected_command,
    int timeout,
    CdmpTransactionCallback callback
) {

    ScopedMutex guard(transactions_lock_);

    uint8_t transaction_id = GetNextTransactionId();

    PendingTransaction transaction{ .transaction_id = transaction_id,
        .start_time = static_cast<uint64_t>(k_uptime_get()),
        .timeout = static_cast<uint64_t>(timeout),
        .callback = std::move(callback),
        .expected_command = expected_command,
        .target_device_id = target_device_id };

    pending_transactions_[transaction_id] = std::move(transaction);

    LOG_DBG("Started transaction %d for command %d", transaction_id, expected_command);

    return transaction_id;
}

bool CdmpTransactionService::IsResponseTo(
    const PendingTransaction& transaction,
    const CdmpCommandResponseMessage& response
) {

    return response.command_code == transaction.expected_command
        && (transaction.target_device_id == CdmpDevice::DEVICE_ID_BROADCAST
            || response.source_device_id == transaction.target_device_id);
}

void CdmpTransactionService::CompleteTransaction(const CdmpCommandResponseMessage& response) {
    Transactions::node_type completed;
    {
        ScopedMutex guard(transactions_lock_);

        auto it = pending_transactions_.find(response.transaction_id);
        if(it == pending_transactions_.end() || !IsResponseTo(it->second, response))
            return;

        completed = pending_transactions_.extract(it);
    }

    InvokeCallback(completed.mapped(), response.result_code, response.data);

    LOG_DBG("Completed transaction %d", response.transaction_id);
}

void CdmpTransactionService::CleanupExpiredTransactions() {
    const uint64_t current_time = k_uptime_get();

    // One at a time, so that each callback runs outside the lock.
    while(true) {
        Transactions::node_type expired;
        {
            ScopedMutex guard(transactions_lock_);

            auto it = std::ranges::find_if(pending_transactions_, [current_time](const auto& entry) {
                return current_time - entry.second.start_time >= entry.second.timeout;
            });
            if(it == pending_transactions_.end())
                return;

            expired = pending_transactions_.extract(it);
        }

        InvokeCallback(expired.mapped(), CdmpResultCode::TIMEOUT, {});

        LOG_DBG("Transaction %d timed out", expired.key());
    }
}

void CdmpTransactionService::InvokeCallback(
    PendingTransaction& transaction,
    CdmpResultCode result_code,
    std::span<const uint8_t> data
) {

    if(transaction.callback)
        transaction.callback(transaction.transaction_id, result_code, data);
}

bool CdmpTransactionService::IsTransactionPending(uint8_t transaction_id) const {
    ScopedMutex guard(transactions_lock_);

    return pending_transactions_.contains(transaction_id);
}

size_t CdmpTransactionService::GetPendingTransactionCount() const {
    ScopedMutex guard(transactions_lock_);

    return pending_transactions_.size();
}

WorkQueueTaskResult CdmpTransactionService::ProcessTransactionTimeouts(CdmpTransactionService* instance) {
    {
        // Rescheduled under the lock, before anything can throw, rather than through the task
        // result: the dispatcher would reschedule after the pass and could arm the timer again
        // while Stop() cancels it.
        ScopedMutex guard(instance->transactions_lock_);

        if(!instance->is_timeout_task_running_)
            return {};

        instance->timeout_task_.value().Reschedule(K_MSEC(CONFIG_EERIE_LEAP_CDMP_CMD_TRANSACTION_TIMEOUT_MS / 2));
    }

    instance->CleanupExpiredTransactions();

    return {};
}

} // namespace eerie_leap::subsys::cdmp::services::command_service
