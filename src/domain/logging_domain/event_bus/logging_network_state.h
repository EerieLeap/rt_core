#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

#include "domain/canbus_com_domain/state_bridge/network_state_descriptor.h"
#include "domain/canbus_com_domain/states/canbus_com_logging_state.h"

#include "logging_event_type.h"
#include "logging_events_channel.h"
#include "logging_payload_type.h"
#include "logging_status_reason.h"

namespace eerie_leap::domain::logging_domain::event_bus {

using eerie_leap::domain::canbus_com_domain::states::CanbusComLoggingState;
using eerie_leap::domain::canbus_com_domain::states::CanbusComStateType;

/**
 * @brief The logging status every unit shares: StatusUpdated on logging_events as the record
 * LOGGING_STATUS, for NetworkStateExporter and NetworkStateImporter.
 *
 * A unit that logs exports it, and a unit that shows the status imports it. Header-only, so a unit
 * without EERIE_LEAP_DOMAIN_LOGGING imports it too.
 *
 * - A unit's record follows its StatusUpdated events, except that an inactive Stopped never replaces
 *   an inactive failure: the owner's stop that cleans up after a failure publishes Stopped, and the
 *   record keeps the failure until the next start.
 * - The combined status is active while any unit logs. Otherwise it is inactive with the failure of
 *   the lowest device ID that reports one (WriteFailed, NoSpace or RotationFailed), else Stopped,
 *   and Unavailable when no unit reports a status.
 */
struct LoggingNetworkState {
    using Channel = LoggingEventsChannel;
    using State = CanbusComLoggingState;

    static constexpr LoggingEventType EVENT_TYPE = LoggingEventType::StatusUpdated;
    static constexpr CanbusComStateType STATE_TYPE = State::STATE_TYPE;

    /** @brief Whether @p reason ends a session that the unit did not stop. */
    static constexpr bool IsFailure(LoggingStatusReason reason) {
        return reason == LoggingStatusReason::WriteFailed
            || reason == LoggingStatusReason::NoSpace
            || reason == LoggingStatusReason::RotationFailed;
    }

    /** @brief A unit that has not published a status yet does not log. */
    static State Initial() {
        return State(false, LoggingStatusReason::Stopped);
    }

    static std::optional<State> FromEvent(const Channel::EventMessage& event, const State& current) {
        if(event.type != EVENT_TYPE)
            return std::nullopt;

        auto active = event.payload.find(LoggingPayloadType::IsActive);
        if(active == event.payload.end())
            return std::nullopt;

        const auto* is_active = std::get_if<bool>(&active->second);
        if(is_active == nullptr)
            return std::nullopt;

        if(*is_active)
            return State(true);

        auto reason = LoggingStatusReason::Stopped;
        if(auto it = event.payload.find(LoggingPayloadType::Reason); it != event.payload.end()) {
            if(const auto* value = std::get_if<uint32_t>(&it->second))
                reason = static_cast<LoggingStatusReason>(*value);
        }

        // Only a start clears a failure.
        if(!IsFailure(reason) && !current.IsActive() && IsFailure(current.GetReason()))
            return std::nullopt;

        return State(false, reason);
    }

    /** @param states Every other unit's record, ordered by device ID. */
    static State Combine(std::span<const State> states) {
        if(states.empty())
            return State(false, LoggingStatusReason::Unavailable);

        if(std::ranges::any_of(states, &State::IsActive))
            return State(true);

        auto failed = std::ranges::find_if(states, [](const State& state) { return IsFailure(state.GetReason()); });

        return State(false, failed != states.end() ? failed->GetReason() : LoggingStatusReason::Stopped);
    }

    /** @brief The event as the log writer publishes it: the reason only while inactive. */
    static Channel::EventMessage ToEvent(const State& state) {
        Channel::EventMessage event {
            .type = EVENT_TYPE
        };

        event.payload.insert_or_assign(LoggingPayloadType::IsActive, state.IsActive());
        if(!state.IsActive())
            event.payload.insert_or_assign(LoggingPayloadType::Reason, static_cast<uint32_t>(state.GetReason()));

        return event;
    }
};

static_assert(eerie_leap::domain::canbus_com_domain::state_bridge::NetworkStateDescriptor<LoggingNetworkState>);

} // namespace eerie_leap::domain::logging_domain::event_bus
