#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "domain/canbus_com_domain/services/canbus_com_service.h"
#include "domain/canbus_com_domain/services/i_network_state_transport.h"
#include "domain/canbus_com_domain/states/canbus_com_state_data.h"
#include "domain/canbus_com_domain/states/canbus_com_state_type.h"
#include "domain/canbus_com_domain/states/i_canbus_com_state.h"

#include "canbus_com_loopback_harness.h"
#include "cdmp_loopback_harness.h"

namespace canbus_com_test {

using namespace cdmp_loopback_test;
using namespace canbus_com_loopback_test;
using namespace eerie_leap::domain::canbus_com_domain::states;

using eerie_leap::domain::canbus_com_domain::services::CanbusComService;
using eerie_leap::domain::canbus_com_domain::services::INetworkStateTransport;

/** @brief A state handler that keeps the records it read on each call, and where it ran. */
class HandlerProbe {
private:
    k_sem called_{};
    k_mutex lock_{};
    std::deque<std::vector<RemoteStateRecord>> snapshots_;
    INetworkStateTransport* transport_ = nullptr;
    CanbusComStateType state_type_{};
    int handler_id_ = -1;
    bool is_off_work_queue_ = false;
    bool is_other_type_ = false;

    void OnStateChanged(CanbusComStateType state_type) {
        const char* thread_name = k_thread_name_get(k_current_get());

        // Read as the importer will, from the handler, with room for every unit's record.
        std::array<RemoteStateRecord, INetworkStateTransport::MAX_REMOTE_RECORDS> records{};
        const size_t count = transport_->GetRemoteStates(state_type, records);

        k_mutex_lock(&lock_, K_FOREVER);
        if(thread_name == nullptr || std::strcmp(thread_name, CDMP_WORK_QUEUE_NAME) != 0)
            is_off_work_queue_ = true;
        if(state_type != state_type_)
            is_other_type_ = true;
        snapshots_.emplace_back(records.begin(), records.begin() + count);
        k_mutex_unlock(&lock_);

        k_sem_give(&called_);
    }

public:
    HandlerProbe() {
        k_sem_init(&called_, 0, K_SEM_MAX_LIMIT);
        k_mutex_init(&lock_);
    }

    void Register(INetworkStateTransport& transport, CanbusComStateType state_type) {
        transport_ = &transport;
        state_type_ = state_type;
        handler_id_ = transport.RegisterStateHandler(state_type, [this](CanbusComStateType type) {
            OnStateChanged(type);
        });
        zassert_true(handler_id_ >= 0, "RegisterStateHandler failed: %d", handler_id_);
    }

    void Unregister() {
        if(handler_id_ < 0)
            return;

        transport_->UnregisterStateHandler(handler_id_);
        handler_id_ = -1;
    }

    /** @brief The records the next call read, or nothing without a call in time. */
    std::optional<std::vector<RemoteStateRecord>> WaitForCall(int timeout_ms = FRAME_TIMEOUT_MS) {
        if(k_sem_take(&called_, K_MSEC(timeout_ms)) != 0)
            return std::nullopt;

        k_mutex_lock(&lock_, K_FOREVER);
        auto snapshot = std::move(snapshots_.front());
        snapshots_.pop_front();
        k_mutex_unlock(&lock_);

        return snapshot;
    }

    /** @brief The records the last of the calls made in time read, or nothing without a call. */
    std::optional<std::vector<RemoteStateRecord>> WaitForLastCall() {
        auto last = WaitForCall();

        // Changes that arrive together may share a call, so the calls are not counted.
        while(auto next = WaitForCall(NO_FRAME_TIMEOUT_MS))
            last = std::move(next);

        return last;
    }

    /** @brief Whether every call ran on the CDMP work queue, with the registered state type. */
    bool CallsWereAsRegistered() {
        k_mutex_lock(&lock_, K_FOREVER);
        const bool is_as_registered = !is_off_work_queue_ && !is_other_type_;
        k_mutex_unlock(&lock_);

        return is_as_registered;
    }
};

} // namespace canbus_com_test
