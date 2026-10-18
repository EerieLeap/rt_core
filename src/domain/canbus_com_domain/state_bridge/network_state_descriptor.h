#pragma once

#include <concepts>
#include <cstdint>
#include <optional>
#include <span>

#include "subsys/event_bus/event_channel.h"
#include "utilities/reflection/caller_name.h"

#include "domain/canbus_com_domain/states/canbus_com_state_type.h"
#include "domain/canbus_com_domain/states/i_canbus_com_state.h"

namespace eerie_leap::domain::canbus_com_domain::state_bridge {

using eerie_leap::domain::canbus_com_domain::states::CanbusComState;
using eerie_leap::domain::canbus_com_domain::states::CanbusComStateType;
using eerie_leap::subsys::event_bus::EventChannel;
using eerie_leap::utilities::reflection::CallerName;
using eerie_leap::utilities::reflection::GetCallerName;

namespace detail {

// The publisher that imported events name, as publishers tag their events: the hash of a function.
consteval CallerName ImportedEventPublisher() {
    return GetCallerName();
}

} // namespace detail

/**
 * @brief The source_id of every event a NetworkStateImporter publishes.
 *
 * Exporters skip events with it, so a unit never shares back what it imported.
 */
inline constexpr uint32_t IMPORTED_EVENT_SOURCE_ID = detail::ImportedEventPublisher().hash;

/**
 * @brief Describes one shared state in the domain that owns it, for NetworkStateExporter and
 * NetworkStateImporter.
 *
 * - `Channel`: the event channel that carries the state, a singleton with `GetInstance()`.
 * - `EVENT_TYPE`: the event type on that channel that carries it.
 * - `State`: the record, a CanbusComState (encodes its data, decodes another unit's record from its
 *   data bytes) that is default constructible, copyable and equality comparable.
 * - `STATE_TYPE`: the record's state type, the one `State::GetStateType()` returns.
 * - `Initial()`: a unit's record before its first event.
 * - `FromEvent(event, current)`: a unit's record after @p event, given its current record, or nothing
 *   when the event does not change it.
 * - `Combine(states)`: the state consumers see, given every other unit's record ordered by device ID.
 *   An empty span means that no unit reports one.
 * - `ToEvent(state)`: the event that carries a combined state. The importer sets its source_id.
 */
template<typename T>
concept NetworkStateDescriptor =
    std::derived_from<
        typename T::Channel,
        EventChannel<typename T::Channel::EventTypeEnum, typename T::Channel::PayloadTypeEnum>>
    && CanbusComState<typename T::State>
    && std::semiregular<typename T::State>
    && std::equality_comparable<typename T::State>
    && requires(
        const T::Channel::EventMessage& event,
        const T::State& state,
        std::span<const typename T::State> states
    ) {
           { T::Channel::GetInstance() } -> std::same_as<typename T::Channel&>;
           { T::EVENT_TYPE } -> std::convertible_to<typename T::Channel::EventTypeEnum>;
           { T::STATE_TYPE } -> std::convertible_to<CanbusComStateType>;
           { T::Initial() } -> std::same_as<typename T::State>;
           { T::FromEvent(event, state) } -> std::same_as<std::optional<typename T::State>>;
           { T::Combine(states) } -> std::same_as<typename T::State>;
           { T::ToEvent(state) } -> std::same_as<typename T::Channel::EventMessage>;
       };

} // namespace eerie_leap::domain::canbus_com_domain::state_bridge
