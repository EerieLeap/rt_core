#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include <zephyr/ztest.h>

#include "network_state_test_support.h"

using namespace network_state_test;

namespace {

using State = LoggingNetworkState::State;

State Active() {
    return State(true);
}

State Inactive(LoggingStatusReason reason) {
    return State(false, reason);
}

LoggingEventsChannel::EventMessage StatusUpdated(bool is_active, std::optional<LoggingStatusReason> reason = std::nullopt) {
    LoggingEventsChannel::EventMessage event {
        .type = LoggingEventType::StatusUpdated
    };

    event.payload.insert_or_assign(LoggingPayloadType::IsActive, is_active);
    if(reason.has_value())
        event.payload.insert_or_assign(LoggingPayloadType::Reason, static_cast<uint32_t>(*reason));

    return event;
}

State Decode(const Bytes& bytes) {
    return State(std::span<const uint8_t>(bytes));
}

State Combine(const std::vector<State>& states) {
    return LoggingNetworkState::Combine(states);
}

constexpr std::array<LoggingStatusReason, 5> ALL_REASONS = {
    LoggingStatusReason::Stopped,
    LoggingStatusReason::WriteFailed,
    LoggingStatusReason::NoSpace,
    LoggingStatusReason::RotationFailed,
    LoggingStatusReason::Unavailable
};

} // namespace

ZTEST_SUITE(logging_network_state, NULL, NULL, NULL, NULL, NULL);

// ======== The record ========

ZTEST(logging_network_state, test_reason_values_are_stable) {
    // The values travel in records and events: appended, never renumbered.
    zassert_equal(static_cast<uint32_t>(LoggingStatusReason::Stopped), 0);
    zassert_equal(static_cast<uint32_t>(LoggingStatusReason::WriteFailed), 1);
    zassert_equal(static_cast<uint32_t>(LoggingStatusReason::NoSpace), 2);
    zassert_equal(static_cast<uint32_t>(LoggingStatusReason::RotationFailed), 3);
    zassert_equal(static_cast<uint32_t>(LoggingStatusReason::Unavailable), 4);
}

ZTEST(logging_network_state, test_record_encodes_flags_and_reason) {
    zassert_equal(Active().GetStateType(), CanbusComStateType::LOGGING_STATUS);
    zassert_equal(std::to_underlying(Active().GetStateType()), 0x10);

    zassert_true(Active().GetData() == Data({0x01, 0x00}), "Active: bit 0 set, no reason");
    zassert_true(Inactive(LoggingStatusReason::Stopped).GetData() == Data({0x00, 0x00}));
    zassert_true(Inactive(LoggingStatusReason::WriteFailed).GetData() == Data({0x00, 0x01}));
    zassert_true(Inactive(LoggingStatusReason::NoSpace).GetData() == Data({0x00, 0x02}));
    zassert_true(Inactive(LoggingStatusReason::RotationFailed).GetData() == Data({0x00, 0x03}));
    zassert_true(Inactive(LoggingStatusReason::Unavailable).GetData() == Data({0x00, 0x04}));

    zassert_true(State(true, LoggingStatusReason::WriteFailed) == Active(), "An active status carries no reason");
    zassert_true(State() == Inactive(LoggingStatusReason::Stopped), "The default is inactive, Stopped");
}

ZTEST(logging_network_state, test_record_round_trips) {
    const State active = Active();
    zassert_true(State(active.GetData().GetData()) == active);
    zassert_true(State(active.GetData().GetData()).IsActive());

    for(const auto reason : ALL_REASONS) {
        const State inactive = Inactive(reason);
        const State decoded(inactive.GetData().GetData());

        zassert_true(decoded == inactive, "Reason %u round-trips", static_cast<unsigned>(reason));
        zassert_false(decoded.IsActive());
        zassert_equal(decoded.GetReason(), reason);
    }
}

ZTEST(logging_network_state, test_decoder_gives_a_missing_reason_its_default) {
    zassert_true(Decode({0x01}) == Active(), "One byte, active");
    zassert_true(Decode({0x00}) == Inactive(LoggingStatusReason::Stopped), "One byte, inactive: Stopped");
    zassert_true(Decode({}) == Inactive(LoggingStatusReason::Stopped), "No byte: the default");
}

ZTEST(logging_network_state, test_decoder_ignores_what_it_does_not_know) {
    zassert_true(Decode({0x00, 0x03, 0xAA, 0xBB}) == Inactive(LoggingStatusReason::RotationFailed),
        "Four bytes: the trailing two are ignored");
    zassert_true(Decode({0x01, 0x02, 0xFF, 0xFF}) == Active(), "Active, whatever follows");
    zassert_true(Decode({0xFE, 0x01}) == Inactive(LoggingStatusReason::WriteFailed), "Flag bits 1-7 are ignored");
    zassert_true(Decode({0xFF, 0x00}) == Active());
}

// ======== The descriptor ========

ZTEST(logging_network_state, test_initial_state_is_inactive_stopped) {
    zassert_true(LoggingNetworkState::Initial() == Inactive(LoggingStatusReason::Stopped));
    zassert_true(LoggingNetworkState::Initial().GetData() == Data({0x00, 0x00}));
}

ZTEST(logging_network_state, test_from_event_maps_the_status) {
    const State stopped = Inactive(LoggingStatusReason::Stopped);

    auto started = LoggingNetworkState::FromEvent(StatusUpdated(true), stopped);
    zassert_true(started.has_value() && *started == Active());

    for(const auto reason : ALL_REASONS) {
        auto inactive = LoggingNetworkState::FromEvent(StatusUpdated(false, reason), Active());
        zassert_true(inactive.has_value() && *inactive == Inactive(reason),
            "Reason %u is kept", static_cast<unsigned>(reason));
    }

    auto without_reason = LoggingNetworkState::FromEvent(StatusUpdated(false), Active());
    zassert_true(without_reason.has_value() && *without_reason == stopped, "No reason reads Stopped");
}

ZTEST(logging_network_state, test_from_event_ignores_events_without_a_status) {
    LoggingEventsChannel::EventMessage empty {
        .type = LoggingEventType::StatusUpdated
    };
    zassert_false(LoggingNetworkState::FromEvent(empty, Active()).has_value(), "No IsActive");

    LoggingEventsChannel::EventMessage wrong_type = empty;
    wrong_type.payload.insert_or_assign(LoggingPayloadType::IsActive, uint32_t{1});
    zassert_false(LoggingNetworkState::FromEvent(wrong_type, Active()).has_value(), "IsActive is not a bool");
}

ZTEST(logging_network_state, test_cleanup_stop_keeps_the_failure) {
    State record = LoggingNetworkState::Initial();

    auto started = LoggingNetworkState::FromEvent(StatusUpdated(true), record);
    zassert_true(started.has_value());
    record = *started;

    auto failed = LoggingNetworkState::FromEvent(StatusUpdated(false, LoggingStatusReason::WriteFailed), record);
    zassert_true(failed.has_value() && *failed == Inactive(LoggingStatusReason::WriteFailed));
    record = *failed;

    // The owner's Stop() after the failure.
    zassert_false(LoggingNetworkState::FromEvent(StatusUpdated(false, LoggingStatusReason::Stopped), record).has_value(),
        "Stopped does not replace WriteFailed");
    zassert_false(LoggingNetworkState::FromEvent(StatusUpdated(false), record).has_value(),
        "Nor does a stop without a reason");

    auto other_failure = LoggingNetworkState::FromEvent(StatusUpdated(false, LoggingStatusReason::NoSpace), record);
    zassert_true(other_failure.has_value() && *other_failure == Inactive(LoggingStatusReason::NoSpace),
        "Another failure does");

    auto restarted = LoggingNetworkState::FromEvent(StatusUpdated(true), record);
    zassert_true(restarted.has_value() && *restarted == Active(), "A start clears the failure");
    record = *restarted;

    auto stopped = LoggingNetworkState::FromEvent(StatusUpdated(false, LoggingStatusReason::Stopped), record);
    zassert_true(stopped.has_value() && *stopped == Inactive(LoggingStatusReason::Stopped),
        "and the next stop is Stopped again");
}

ZTEST(logging_network_state, test_combine_without_records_is_unavailable) {
    zassert_true(Combine({}) == Inactive(LoggingStatusReason::Unavailable));
}

ZTEST(logging_network_state, test_combine_is_active_while_any_unit_logs) {
    zassert_true(Combine({Active()}) == Active());
    zassert_true(Combine({
        Inactive(LoggingStatusReason::WriteFailed),
        Inactive(LoggingStatusReason::Stopped),
        Active()}) == Active(), "A failure of a lower ID does not hide a unit that logs");
}

ZTEST(logging_network_state, test_combine_prefers_the_failure_of_the_lowest_id) {
    zassert_true(Combine({Inactive(LoggingStatusReason::Stopped)}) == Inactive(LoggingStatusReason::Stopped));
    zassert_true(Combine({
        Inactive(LoggingStatusReason::Stopped),
        Inactive(LoggingStatusReason::Stopped)}) == Inactive(LoggingStatusReason::Stopped));

    zassert_true(Combine({
        Inactive(LoggingStatusReason::Stopped),
        Inactive(LoggingStatusReason::NoSpace)}) == Inactive(LoggingStatusReason::NoSpace),
        "A failure is preferred over Stopped");

    // The records come ordered by device ID, so the first failure is the lowest ID's, whatever the
    // reason's value.
    zassert_true(Combine({
        Inactive(LoggingStatusReason::Stopped),
        Inactive(LoggingStatusReason::RotationFailed),
        Inactive(LoggingStatusReason::WriteFailed)}) == Inactive(LoggingStatusReason::RotationFailed),
        "The lowest ID wins over a lower reason");
    zassert_true(Combine({
        Inactive(LoggingStatusReason::Stopped),
        Inactive(LoggingStatusReason::WriteFailed),
        Inactive(LoggingStatusReason::RotationFailed)}) == Inactive(LoggingStatusReason::WriteFailed),
        "and over a higher one");

    zassert_true(Combine({
        Inactive(LoggingStatusReason::Unavailable),
        Inactive(static_cast<LoggingStatusReason>(0x7F))}) == Inactive(LoggingStatusReason::Stopped),
        "A reason that is not a failure counts as Stopped");
}

ZTEST(logging_network_state, test_to_event_is_published_as_the_log_writer_does) {
    auto active = LoggingNetworkState::ToEvent(Active());
    zassert_equal(active.type, LoggingEventType::StatusUpdated);
    zassert_equal(active.source_id, 0, "The importer sets the source");
    zassert_true(std::get<bool>(active.payload.at(LoggingPayloadType::IsActive)));
    zassert_false(active.payload.contains(LoggingPayloadType::Reason), "No reason while active");

    auto unavailable = LoggingNetworkState::ToEvent(Inactive(LoggingStatusReason::Unavailable));
    zassert_equal(unavailable.type, LoggingEventType::StatusUpdated);
    zassert_false(std::get<bool>(unavailable.payload.at(LoggingPayloadType::IsActive)));
    zassert_equal(std::get<uint32_t>(unavailable.payload.at(LoggingPayloadType::Reason)),
        static_cast<uint32_t>(LoggingStatusReason::Unavailable));

    // What a unit imports is what its consumers would map back.
    for(const auto reason : ALL_REASONS) {
        auto mapped = LoggingNetworkState::FromEvent(LoggingNetworkState::ToEvent(Inactive(reason)), Active());
        zassert_true(mapped.has_value() && *mapped == Inactive(reason));
    }
}
