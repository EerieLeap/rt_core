#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

#include "domain/canbus_com_domain/services/i_network_state_transport.h"

#include "network_state_descriptor.h"

namespace eerie_leap::domain::canbus_com_domain::state_bridge {

using eerie_leap::domain::canbus_com_domain::services::INetworkStateTransport;
using eerie_leap::domain::canbus_com_domain::states::RemoteStateRecord;

/**
 * @brief Publishes the state the other units share on the descriptor's channel, on a unit that
 * consumes it.
 *
 * Whenever the other units' records of the descriptor's state type change, and once right after
 * construction so that it catches up, it decodes every unit's record, combines them with
 * `Combine()` and publishes `ToEvent()` of the result, but only when that differs from the state it
 * published last; while no bus carries the channel it publishes nothing and counts nothing as
 * published. The events carry IMPORTED_EVENT_SOURCE_ID and are published with PublishAsync(), so
 * consumers run on the channel's bus thread and cannot tell them from local events. A unit either
 * exports or imports a given descriptor, never both. Destruction stops it.
 *
 * Records are handled on the CDMP work queue, one change at a time.
 */
template<NetworkStateDescriptor TDescriptor>
class NetworkStateImporter {
public:
    using Descriptor = TDescriptor;
    using State = typename TDescriptor::State;
    using Channel = typename TDescriptor::Channel;
    using EventMessage = typename Channel::EventMessage;

private:
    std::shared_ptr<INetworkStateTransport> transport_;
    int handler_id_ = -1;

    // Only the handler touches these, and the CDMP work queue runs one handler at a time. They are
    // members so that every unit's record stays off the work queue's stack.
    std::array<RemoteStateRecord, INetworkStateTransport::MAX_REMOTE_RECORDS> records_{};
    std::array<State, INetworkStateTransport::MAX_REMOTE_RECORDS> states_{};
    std::optional<State> published_;

    void OnStatesChanged() {
        const size_t count = transport_->GetRemoteStates(TDescriptor::STATE_TYPE, records_);

        for(size_t i = 0; i < count; i++)
            states_[i] = State(records_[i].data.GetData());

        State combined = TDescriptor::Combine(std::span<const State>(states_.data(), count));
        if(published_.has_value() && *published_ == combined)
            return;

        // A channel no bus carries drops the event, so it does not count as published: the first
        // call after a bus registers the channel publishes the state, even an unchanged one.
        Channel& channel = Channel::GetInstance();
        if(!channel.IsRegistered())
            return;

        EventMessage event = TDescriptor::ToEvent(combined);
        event.source_id = IMPORTED_EVENT_SOURCE_ID;

        published_ = std::move(combined);

        // A handler must not block; consumers run on the channel's bus thread.
        channel.PublishAsync(event);
    }

public:
    /**
     * @brief Registers for the descriptor's state type; the first combined state follows on the CDMP
     * work queue.
     *
     * A bus should carry the channel by then. Until one does, nothing is published, and the combined
     * state goes out with the next change of a record after a bus registers the channel.
     *
     * @throws std::runtime_error if the transport refuses the handler.
     */
    explicit NetworkStateImporter(std::shared_ptr<INetworkStateTransport> transport) :
        transport_(std::move(transport)),
        handler_id_(
            transport_->RegisterStateHandler(TDescriptor::STATE_TYPE, [this](CanbusComStateType /*state_type*/) {
                OnStatesChanged();
            })
        ) {

        if(handler_id_ < 0)
            throw std::runtime_error("Network state importer failed to register its state handler");
    }

    /**
     * @brief Once it returns, the handler is not running and publishes nothing more; events already
     * queued are still delivered. Not from a state handler.
     */
    ~NetworkStateImporter() {
        transport_->UnregisterStateHandler(handler_id_);
    }

    NetworkStateImporter(const NetworkStateImporter&) = delete;
    NetworkStateImporter& operator=(const NetworkStateImporter&) = delete;
    NetworkStateImporter(NetworkStateImporter&&) = delete;
    NetworkStateImporter& operator=(NetworkStateImporter&&) = delete;
};

} // namespace eerie_leap::domain::canbus_com_domain::state_bridge
