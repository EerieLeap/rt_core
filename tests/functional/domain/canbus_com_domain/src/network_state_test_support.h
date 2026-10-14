#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "subsys/event_bus/event_bus.h"
#include "subsys/event_bus/event_channel.h"
#include "subsys/threading/scoped_mutex.h"

#include "domain/canbus_com_domain/services/i_network_state_transport.h"
#include "domain/canbus_com_domain/state_bridge/network_state_descriptor.h"
#include "domain/canbus_com_domain/state_bridge/network_state_exporter.h"
#include "domain/canbus_com_domain/state_bridge/network_state_importer.h"
#include "domain/canbus_com_domain/states/canbus_com_logging_state.h"
#include "domain/canbus_com_domain/states/canbus_com_state_data.h"
#include "domain/canbus_com_domain/states/canbus_com_state_type.h"
#include "domain/logging_domain/event_bus/logging_events_channel.h"
#include "domain/logging_domain/event_bus/logging_network_state.h"

// The state bridge with the logging descriptor, as the units use it. This suite builds neither the
// logging domain nor its bus, as on the gauge, which imports the logging status without them.
namespace network_state_test {

using namespace eerie_leap::domain::canbus_com_domain::state_bridge;
using namespace eerie_leap::domain::canbus_com_domain::states;
using namespace eerie_leap::domain::logging_domain::event_bus;

using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::event_bus::CreateScopedSubscription;
using eerie_leap::subsys::event_bus::EventBus;
using eerie_leap::subsys::event_bus::EventChannel;
using eerie_leap::subsys::threading::ScopedMutex;
using eerie_leap::domain::canbus_com_domain::services::INetworkStateTransport;

using Bytes = std::vector<uint8_t>;
using LoggingExporter = NetworkStateExporter<LoggingNetworkState>;
using LoggingImporter = NetworkStateImporter<LoggingNetworkState>;

inline constexpr int EVENT_TIMEOUT_MS = 1000;
// Well past the time an event takes through the bus.
inline constexpr int NO_EVENT_TIMEOUT_MS = 100;

/** @brief What a fence event carries, so that StatusProbe::Flush() recognises it. */
inline constexpr uint32_t FENCE_SOURCE_ID = 0xFE7CE;

inline CanbusComStateData Data(const Bytes& bytes) {
    return CanbusComStateData(bytes);
}

/** @brief A bus for logging_events, which drops every publish until a bus registers it. */
class BridgeTestBus : public EventBus {
public:
    BridgeTestBus() : EventBus("bridge_test_bus", 4096) {
        zassert_ok(RegisterChannel(LoggingEventsChannel::GetInstance()));
    }

    /** @brief The one bus of the suite; a channel stays with the first bus that registers it. */
    static BridgeTestBus& GetInstance() {
        static BridgeTestBus bus;

        return bus;
    }
};

/** @brief Publishes a status as the log writer does: asynchronously, the reason only while inactive. */
inline void PublishStatus(
    bool is_active,
    std::optional<LoggingStatusReason> reason = std::nullopt,
    uint32_t source_id = 0) {

    LoggingEventsChannel::EventMessage event {
        .source_id = source_id,
        .type = LoggingEventType::StatusUpdated
    };

    event.payload.insert_or_assign(LoggingPayloadType::IsActive, is_active);
    if(reason.has_value())
        event.payload.insert_or_assign(LoggingPayloadType::Reason, static_cast<uint32_t>(*reason));

    LoggingEventsChannel::GetInstance().PublishAsync(event);
}

/** @brief A StatusUpdated event as a consumer receives it. */
struct StatusEvent {
    uint32_t source_id = 0;
    std::optional<bool> is_active;
    std::optional<uint32_t> reason;
};

/** @brief A channel with the events of logging_events. */
using LoggingChannel = EventChannel<LoggingEventType, LoggingPayloadType>;

/** @brief Keeps the StatusUpdated events published on logging_events, as a consumer sees them. */
class StatusProbe {
private:
    // Shared with the subscription, which a dispatch in flight may still call after the probe is gone.
    struct Inbox {
        k_sem received{};
        k_mutex lock{};
        std::deque<StatusEvent> events;

        Inbox() {
            k_sem_init(&received, 0, K_SEM_MAX_LIMIT);
            k_mutex_init(&lock);
        }
    };

    LoggingChannel& channel_;
    std::shared_ptr<Inbox> inbox_ = std::make_shared<Inbox>();
    AnySubscription subscription_;

public:
    explicit StatusProbe(LoggingChannel& channel = LoggingEventsChannel::GetInstance()) : channel_(channel) {
        subscription_ = CreateScopedSubscription(
            channel_,
            LoggingEventType::StatusUpdated,
            [inbox = inbox_](const LoggingEventsChannel::EventMessage& event) {
                StatusEvent status{.source_id = event.source_id};

                if(auto it = event.payload.find(LoggingPayloadType::IsActive); it != event.payload.end()) {
                    if(const auto* value = std::get_if<bool>(&it->second))
                        status.is_active = *value;
                }

                if(auto it = event.payload.find(LoggingPayloadType::Reason); it != event.payload.end()) {
                    if(const auto* value = std::get_if<uint32_t>(&it->second))
                        status.reason = *value;
                }

                {
                    ScopedMutex guard(inbox->lock);
                    inbox->events.push_back(status);
                }

                k_sem_give(&inbox->received);
            });

        zassert_not_null(subscription_.get(), "The probe subscribes to %s", channel_.GetName());
    }

    /** @brief The next event, or nothing without one in time. */
    std::optional<StatusEvent> WaitForEvent(int timeout_ms = EVENT_TIMEOUT_MS) {
        if(k_sem_take(&inbox_->received, K_MSEC(timeout_ms)) != 0)
            return std::nullopt;

        ScopedMutex guard(inbox_->lock);
        auto event = inbox_->events.front();
        inbox_->events.pop_front();

        return event;
    }

    /**
     * @brief Drops every event published so far: the channel delivers in order, so everything before
     * a fence event arrives before it.
     */
    void Flush() {
        channel_.PublishAsync({
            .source_id = FENCE_SOURCE_ID,
            .type = LoggingEventType::StatusUpdated
        });

        while(true) {
            auto event = WaitForEvent();
            zassert_true(event.has_value(), "The fence event arrives");

            if(event->source_id == FENCE_SOURCE_ID)
                return;
        }
    }
};

/**
 * @brief An INetworkStateTransport that keeps the changes of this unit's records and serves the other
 * units' records that the test sets.
 *
 * CanbusComService makes a handler's first call on the CDMP work queue; the fake makes it at once, on
 * the registering thread, and the later ones from NotifyHandlers(), on the test's thread.
 */
class FakeTransport : public INetworkStateTransport {
public:
    /**
     * @brief A SetLocalState() or, with empty data, a ClearLocalState(). A refused SetLocalState()
     * is kept too, so that a test can wait for it.
     */
    struct LocalChange {
        CanbusComStateType state_type{};
        CanbusComStateData data;
        bool is_accepted = true;
    };

private:
    struct HandlerEntry {
        CanbusComStateType state_type{};
        StateHandler handler;
    };

    mutable k_mutex lock_{};
    k_sem changed_{};
    std::deque<LocalChange> local_changes_;
    bool is_refusing_ = false;
    // Other units' records by type, then device ID, so that each type's are in device order.
    std::map<std::pair<CanbusComStateType, uint8_t>, CanbusComStateData> remote_states_;
    std::map<int, HandlerEntry> handlers_;
    int next_handler_id_ = 0;

    bool AddLocalChange(CanbusComStateType state_type, const CanbusComStateData& data, bool can_refuse) {
        bool is_accepted = true;
        {
            ScopedMutex guard(lock_);
            is_accepted = !(can_refuse && is_refusing_);
            local_changes_.push_back({.state_type = state_type, .data = data, .is_accepted = is_accepted});
        }

        k_sem_give(&changed_);

        return is_accepted;
    }

public:
    FakeTransport() {
        k_mutex_init(&lock_);
        k_sem_init(&changed_, 0, K_SEM_MAX_LIMIT);
    }

    bool SetLocalState(CanbusComStateType state_type, const CanbusComStateData& data) override {
        return AddLocalChange(state_type, data, true);
    }

    using INetworkStateTransport::SetLocalState;

    void ClearLocalState(CanbusComStateType state_type) override {
        AddLocalChange(state_type, CanbusComStateData(), false);
    }

    /** @brief Makes SetLocalState() fail, as when every record slot holds another state type. */
    void RefuseLocalStates(bool is_refusing) {
        ScopedMutex guard(lock_);

        is_refusing_ = is_refusing;
    }

    int RegisterStateHandler(CanbusComStateType state_type, StateHandler handler) override {
        int handler_id = 0;
        {
            ScopedMutex guard(lock_);
            handler_id = next_handler_id_++;
            handlers_.emplace(handler_id, HandlerEntry{.state_type = state_type, .handler = handler});
        }

        handler(state_type);

        return handler_id;
    }

    void UnregisterStateHandler(int handler_id) override {
        ScopedMutex guard(lock_);

        handlers_.erase(handler_id);
    }

    size_t GetRemoteStates(CanbusComStateType state_type, std::span<RemoteStateRecord> records) const override {
        ScopedMutex guard(lock_);

        size_t count = 0;
        for(const auto& [key, data] : remote_states_) {
            if(count == records.size())
                break;

            if(key.first == state_type)
                records[count++] = {.device_id = key.second, .data = data};
        }

        return count;
    }

    /** @brief Sets another unit's record, without telling the handlers. */
    void SetRemoteState(uint8_t device_id, const Bytes& data, CanbusComStateType state_type = CanbusComStateType::LOGGING_STATUS) {
        ScopedMutex guard(lock_);

        remote_states_.insert_or_assign({state_type, device_id}, Data(data));
    }

    /** @brief Removes another unit's record, without telling the handlers. */
    void RemoveRemoteState(uint8_t device_id, CanbusComStateType state_type = CanbusComStateType::LOGGING_STATUS) {
        ScopedMutex guard(lock_);

        remote_states_.erase({state_type, device_id});
    }

    /** @brief Calls the handlers of @p state_type, as a change of its records does. */
    void NotifyHandlers(CanbusComStateType state_type = CanbusComStateType::LOGGING_STATUS) {
        std::vector<StateHandler> handlers;
        {
            ScopedMutex guard(lock_);
            for(const auto& [_, entry] : handlers_) {
                if(entry.state_type == state_type)
                    handlers.push_back(entry.handler);
            }
        }

        for(const auto& handler : handlers)
            handler(state_type);
    }

    [[nodiscard]] size_t GetHandlerCount() const {
        ScopedMutex guard(lock_);

        return handlers_.size();
    }

    /** @brief The next change of this unit's records, or nothing without one in time. */
    std::optional<LocalChange> WaitForLocalChange(int timeout_ms = EVENT_TIMEOUT_MS) {
        if(k_sem_take(&changed_, K_MSEC(timeout_ms)) != 0)
            return std::nullopt;

        ScopedMutex guard(lock_);
        auto change = local_changes_.front();
        local_changes_.pop_front();

        return change;
    }
};

} // namespace network_state_test
