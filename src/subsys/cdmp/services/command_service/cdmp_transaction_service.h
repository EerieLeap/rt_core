#pragma once

#include <memory>
#include <optional>

#include "subsys/threading/work_queue_thread.h"

#include "subsys/cdmp/models/cdmp_device.h"
#include "subsys/cdmp/models/cdmp_message.h"

#include <functional>
#include <unordered_map>
#include <zephyr/kernel.h>

namespace eerie_leap::subsys::cdmp::services::command_service {

namespace threading = eerie_leap::subsys::threading;

using eerie_leap::subsys::cdmp::models::CdmpCommandResponseMessage;
using eerie_leap::subsys::cdmp::models::CdmpDevice;
using eerie_leap::subsys::cdmp::utilities::CdmpResultCode;
using threading::WorkQueueTaskResult;
using threading::WorkQueueThread;

using CdmpTransactionCallback =
    std::function<void(uint8_t transaction_id, const CdmpResultCode, std::span<const uint8_t>)>;

struct PendingTransaction {
    uint8_t transaction_id = 0;
    uint64_t start_time = 0;
    uint64_t timeout = 0;
    CdmpTransactionCallback callback;
    uint8_t expected_command = 0;
    uint8_t target_device_id = 0; ///< DEVICE_ID_BROADCAST accepts a response from any device.
};

class CdmpTransactionService {
private:
    using Transactions = std::unordered_map<uint8_t, PendingTransaction>;

    std::shared_ptr<WorkQueueThread> work_queue_thread_;
    std::shared_ptr<CdmpDevice> device_;

    std::optional<threading::WorkQueueTask<CdmpTransactionService>> timeout_task_;
    static WorkQueueTaskResult ProcessTransactionTimeouts(CdmpTransactionService* instance);

    // Transactions start on any thread and complete or expire on the CDMP work queue, while Stop()
    // runs on the thread that changes the device status. Callbacks run outside the lock.
    mutable k_mutex transactions_lock_{};
    // The timeout task reschedules itself under the lock while this is set.
    bool is_timeout_task_running_ = false;
    uint8_t next_transaction_id_ = 1;
    Transactions pending_transactions_;

    void CleanupExpiredTransactions();
    uint8_t GetNextTransactionId();
    static bool IsResponseTo(const PendingTransaction& transaction, const CdmpCommandResponseMessage& response);
    static void
    InvokeCallback(PendingTransaction& transaction, CdmpResultCode result_code, std::span<const uint8_t> data);

public:
    CdmpTransactionService(std::shared_ptr<WorkQueueThread> work_queue_thread, std::shared_ptr<CdmpDevice> device);

    ~CdmpTransactionService();
    CdmpTransactionService(const CdmpTransactionService&) = delete;
    CdmpTransactionService& operator=(const CdmpTransactionService&) = delete;
    CdmpTransactionService(CdmpTransactionService&&) = delete;
    CdmpTransactionService& operator=(CdmpTransactionService&&) = delete;

    void Initialize();
    void Start();
    /** @brief Stops the timeouts and cancels every pending transaction. */
    void Stop();

    uint8_t StartTransaction(
        uint8_t target_device_id,
        uint8_t expected_command,
        int timeout = CONFIG_EERIE_LEAP_CDMP_CMD_TRANSACTION_TIMEOUT_MS,
        CdmpTransactionCallback callback = nullptr
    );
    /**
     * @brief Completes the transaction @p response answers, if any.
     *
     * Every unit numbers its own transactions and responses are broadcast, so a response completes a
     * transaction only if it also echoes its command and comes from its target, or from any device
     * when the request was broadcast.
     */
    void CompleteTransaction(const CdmpCommandResponseMessage& response);

    bool IsTransactionPending(uint8_t transaction_id) const;
    size_t GetPendingTransactionCount() const;
};

} // namespace eerie_leap::subsys::cdmp::services::command_service
