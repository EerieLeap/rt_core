#pragma once

#include <memory>
#include <optional>

#include "subsys/threading/work_queue_thread.h"

#include "subsys/cdmp/models/cdmp_device.h"
#include "subsys/cdmp/models/cdmp_message.h"

#include <unordered_map>
#include <functional>
#include <zephyr/kernel.h>

namespace eerie_leap::subsys::cdmp::services::command_service {

namespace threading = eerie_leap::subsys::threading;

using threading::WorkQueueThread;
using threading::WorkQueueTaskResult;
using eerie_leap::subsys::cdmp::utilities::CdmpResultCode;
using eerie_leap::subsys::cdmp::models::CdmpDevice;
using eerie_leap::subsys::cdmp::models::CdmpCommandResponseMessage;

using CdmpTransactionCallback = std::function<void(uint8_t transaction_id, const CdmpResultCode, std::span<const uint8_t>)>;

struct PendingTransaction {
    uint8_t transaction_id;
    uint64_t start_time;
    uint64_t timeout;
    CdmpTransactionCallback callback;
    uint8_t expected_command;
};

class CdmpTransactionService {
private:
    std::shared_ptr<WorkQueueThread> work_queue_thread_;
    std::shared_ptr<CdmpDevice> device_;

    std::optional<threading::WorkQueueTask<CdmpTransactionService>> timeout_task_;
    bool is_timeout_task_running_ = false;
    static WorkQueueTaskResult ProcessTransactionTimeouts(CdmpTransactionService* instance);

    uint8_t next_transaction_id_ = 1;
    std::unordered_map<uint8_t, PendingTransaction> pending_transactions_;

    void CleanupExpiredTransactions();
    void CancelTransaction(uint8_t transaction_id);
    uint8_t GetNextTransactionId();

public:
    CdmpTransactionService(
        std::shared_ptr<WorkQueueThread> work_queue_thread,
        std::shared_ptr<CdmpDevice> device);

    ~CdmpTransactionService();

    void Initialize();
    void Start();
    void Stop();

    uint8_t StartTransaction(
        uint8_t expected_command,
        int timeout = CONFIG_EERIE_LEAP_CDMP_CMD_TRANSACTION_TIMEOUT_MS,
        CdmpTransactionCallback callback = nullptr);
    void CompleteTransaction(const CdmpCommandResponseMessage& response);

    bool IsTransactionPending(uint8_t transaction_id) const;
    size_t GetPendingTransactionCount() const { return pending_transactions_.size(); }
};

} // namespace eerie_leap::subsys::cdmp::services::command_service
