#pragma once

#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <utility>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "domain/canbus_com_domain/commands/canbus_com_command_code.h"

#include "canbus_com_loopback_harness.h"
#include "cdmp_test_features.h"

using eerie_leap::tests::LOGGER_FEATURES;

// The restart of a scripted logging unit on the loopback harness, for the suites that exercise the
// restart monitor and the re-assertion it feeds. Everything has external linkage on purpose.
namespace logging_restart_test {

using namespace canbus_com_loopback_test;

using eerie_leap::domain::canbus_com_domain::commands::CanbusComCommandCode;

inline constexpr uint8_t k_logging_command_code = std::to_underlying(CanbusComCommandCode::LOGGING);

// A LOGGING command on the bus: the broadcast, the code, the transaction, then start or stop.
inline bool IsLoggingBroadcastFrame(const Bytes& frame) {
    return frame.size() == 4 && frame[0] == CdmpDevice::DEVICE_ID_BROADCAST && frame[1] == k_logging_command_code;
}

// A LOGGING command to any unit: the broadcast, or a unicast to one.
inline bool IsLoggingCommandFrame(const Bytes& frame) {
    return frame.size() == 4 && frame[1] == k_logging_command_code;
}

// A LOGGING command addressed to @p device_id alone.
inline bool IsLoggingCommandTo(const Bytes& frame, uint8_t device_id) {
    return IsLoggingCommandFrame(frame) && frame[0] == device_id;
}

// What a unit that reboots sends: its ID claim again with the same UID, which the unit reports as a
// restart, then its first heartbeat with the digest of the records it holds. The frames only: the
// unit takes them on the CDMP work queue, in this order. A digest of no records is a unit that has
// not announced any yet.
inline void SendRestart(Harness& harness, uint8_t device_id, uint32_t uid, std::initializer_list<Record> records) {
    harness.SendAsPeer(MANAGEMENT, CdmpIdClaimRequestMessage{
        .claiming_device_id = device_id,
        .uid = uid,
        .protocol_version = CdmpDevice::GetCurrentProtocolVersion()}.ToCanFrame());
    harness.SendAsPeer(HEARTBEAT, HeartbeatFrame(device_id, Digest(records), LOGGER_FEATURES));
}

// Returns once the unit has taken what SendRestart() sent, and whether it sent a LOGGING command
// meanwhile, which it must not before the restarted unit can accept it: tests assert it is false.
//
// A unit that finds its copies different from the digest asks the restarted unit to replay its
// records; that is the barrier then. The request is rate limited per unit (once per 1.5 s), so a case
// says when one is due: the first restart of a unit in the case, with a digest that differs from the
// unit's copies. Otherwise nothing on the bus shows that the heartbeat was taken, and the barrier is
// the length of the absence window, past the time a frame takes through the loopback controller. A
// record a unit sends before its claim has been processed is a duplicate of the stale copy, not a
// renewal of it, so the caller shares the new record only after this returns.
[[nodiscard]] inline bool WaitForRestartTaken(Harness& harness, uint8_t device_id, bool is_sync_request_due) {
    if(is_sync_request_due) {
        // One matcher for both frames: a wait for the request alone would drop a broadcast on the way.
        const auto frame = harness.bus.Wait(COMMAND, [device_id](const Bytes& bytes) {
            return IsLoggingCommandFrame(bytes) || IsSyncRequestTo(device_id)(bytes);
        });
        zassert_true(frame.has_value(),
            "The unit asks the restarted unit to replay its records, as its digest differs");
        if(IsLoggingCommandFrame(frame->data))
            return true;
    }

    return harness.bus.Wait(COMMAND, IsLoggingCommandFrame, NO_FRAME_TIMEOUT_MS).has_value();
}

// Holds the CDMP work queue inside a heartbeat handler, so that the frames the scripted peer sends
// meanwhile queue up behind it, as they do on a busy bus: the unit then takes the records first, any
// heartbeat after them, and runs its state handlers last. Close() sends the heartbeat that gets the
// queue into the handler, Open() lets go. The hold gives up after 5 s, so a case that never opens it
// cannot wedge the queue for good.
class CdmpQueueHold {
private:
    struct Context {
        k_sem entered;
        k_sem open;
        std::atomic<bool> is_armed = false;

        Context() {
            k_sem_init(&entered, 0, 1);
            k_sem_init(&open, 0, 1);
        }
    };

    Harness& harness_;
    std::shared_ptr<Context> context_ = std::make_shared<Context>();
    int handler_id_ = -1;
    bool is_closed_ = false;

public:
    // The handler registers after the monitor's and the state service's, so it holds the queue only
    // after they ran for the heartbeat that closes the hold.
    explicit CdmpQueueHold(Harness& harness) : harness_(harness) {
        handler_id_ = harness_.com->RegisterHeartbeatHandler([context = context_](const CdmpHeartbeatMessage&) {
            if(!context->is_armed.exchange(false))
                return;

            k_sem_give(&context->entered);
            k_sem_take(&context->open, K_SECONDS(5));
        });
        zassert_true(handler_id_ >= 0, "The hold's heartbeat handler was refused");
    }

    ~CdmpQueueHold() {
        Open();
        harness_.com->UnregisterHeartbeatHandler(handler_id_);
    }

    CdmpQueueHold(const CdmpQueueHold&) = delete;
    CdmpQueueHold& operator=(const CdmpQueueHold&) = delete;

    // @p device_id is a unit in the table: its heartbeat, with @p state_digest, is the one the queue
    // stops in. Returns once it does.
    [[nodiscard]] bool Close(uint8_t device_id, uint8_t state_digest = 0) {
        Open();
        k_sem_reset(&context_->entered);
        k_sem_reset(&context_->open);

        context_->is_armed = true;
        is_closed_ = true;
        harness_.SendAsPeer(HEARTBEAT, HeartbeatFrame(device_id, state_digest, LOGGER_FEATURES));

        return k_sem_take(&context_->entered, K_SECONDS(5)) == 0;
    }

    // Does nothing while the queue is not held.
    void Open() {
        if(!is_closed_)
            return;

        is_closed_ = false;
        k_sem_give(&context_->open);
    }
};

} // namespace logging_restart_test
