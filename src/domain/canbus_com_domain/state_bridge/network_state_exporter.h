#pragma once

#include <memory>
#include <stdexcept>
#include <utility>

#include <zephyr/kernel.h>

#include "subsys/event_bus/event_channel.h"
#include "subsys/event_bus/i_scoped_subscription.h"
#include "subsys/threading/scoped_mutex.h"

#include "domain/canbus_com_domain/services/i_network_state_transport.h"

#include "network_state_descriptor.h"

namespace eerie_leap::domain::canbus_com_domain::state_bridge {

using eerie_leap::domain::canbus_com_domain::services::INetworkStateTransport;
using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::event_bus::CreateScopedSubscription;
using eerie_leap::subsys::threading::ScopedMutex;

/**
 * @brief Shares this unit's state on the descriptor's channel with the other units, on a unit that
 * produces it.
 *
 * Shares `Initial()` on construction, then maps each event of the descriptor's type through
 * `FromEvent(event, current)` and shares the result when it differs from the current record. A
 * record the transport refuses is offered again with each later event. Events an importer published
 * (IMPORTED_EVENT_SOURCE_ID) are skipped, so a unit never shares back what it imported; a unit either
 * exports or imports a given descriptor, never both. Destruction withdraws the record.
 *
 * Events are handled on the thread of the bus that carries the channel.
 */
template<NetworkStateDescriptor TDescriptor>
class NetworkStateExporter {
public:
    using Descriptor = TDescriptor;
    using State = TDescriptor::State;
    using Channel = TDescriptor::Channel;
    using EventMessage = Channel::EventMessage;

private:
    // Shared with the subscription, whose handler a dispatch that began before the exporter
    // unsubscribed may still run.
    struct Record {
        std::shared_ptr<INetworkStateTransport> transport;
        k_mutex lock{};
        // This unit's record, which each event is mapped against.
        State current;
        // Whether the transport took `current`. A record it refused (every record slot holds another
        // state type) is offered again with each later event, so it goes out once a slot is free.
        bool is_shared = false;
        bool is_withdrawn = false;

        explicit Record(std::shared_ptr<INetworkStateTransport> record_transport) :
            transport(std::move(record_transport)),
            current(TDescriptor::Initial()),
            is_shared(transport->SetLocalState(current)) {

            k_mutex_init(&lock);

            // Before the exporter subscribes, so that no event's record is replaced by the initial one.
        }

        void Update(const EventMessage& event) {
            ScopedMutex guard(lock);

            if(is_withdrawn)
                return;

            auto next = TDescriptor::FromEvent(event, current);
            if(next.has_value() && *next != current) {
                current = std::move(*next);
                is_shared = false;
            }

            if(!is_shared)
                is_shared = transport->SetLocalState(current);
        }

        void Withdraw() {
            ScopedMutex guard(lock);

            is_withdrawn = true;
            transport->ClearLocalState(TDescriptor::STATE_TYPE);
        }
    };

    std::shared_ptr<Record> record_;
    AnySubscription subscription_;

public:
    /**
     * @brief Shares `Initial()` and subscribes to the descriptor's channel.
     *
     * A bus should carry the channel by then: until one does, the channel drops its events, and the
     * unit keeps sharing `Initial()`.
     *
     * @throws std::runtime_error if the channel refuses the subscription; the record is withdrawn.
     */
    explicit NetworkStateExporter(std::shared_ptr<INetworkStateTransport> transport) :
        record_(std::make_shared<Record>(std::move(transport))),
        subscription_(CreateScopedSubscription(
            Channel::GetInstance(),
            TDescriptor::EVENT_TYPE,
            [](const EventMessage& event) {
                return event.source_id != IMPORTED_EVENT_SOURCE_ID;
            },
            [record = record_](const EventMessage& event) {
                record->Update(event);
            }
        )) {

        if(subscription_ == nullptr) {
            record_->Withdraw();
            throw std::runtime_error("Network state exporter failed to subscribe");
        }
    }

    /** @brief Unsubscribes and withdraws the record; an event in flight no longer changes it. */
    ~NetworkStateExporter() {
        subscription_.reset();
        record_->Withdraw();
    }

    NetworkStateExporter(const NetworkStateExporter&) = delete;
    NetworkStateExporter& operator=(const NetworkStateExporter&) = delete;
    NetworkStateExporter(NetworkStateExporter&&) = delete;
    NetworkStateExporter& operator=(NetworkStateExporter&&) = delete;
};

} // namespace eerie_leap::domain::canbus_com_domain::state_bridge
