#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "domain/logging_domain/event_bus/logging_status_reason.h"

#include "i_canbus_com_state.h"

namespace eerie_leap::domain::canbus_com_domain::states {

using eerie_leap::domain::logging_domain::event_bus::LoggingStatusReason;

/**
 * @brief A unit's logging status (LOGGING_STATUS, 0x10), the counterpart of CanbusComLoggingCommand.
 *
 * Two bytes:
 *
 * - byte 0, flags: bit 0 active, bits 1-7 zero;
 * - byte 1: the LoggingStatusReason while inactive, 0 while active.
 *
 * The decoder ignores the other flag bits and any byte after byte 1, and a record of one byte is
 * inactive for the reason Stopped. An active status carries no reason, so it always reads Stopped.
 */
class CanbusComLoggingState : public ICanbusComState {
public:
    static constexpr CanbusComStateType STATE_TYPE = CanbusComStateType::LOGGING_STATUS;
    static constexpr uint8_t FLAG_ACTIVE = 0x01;

private:
    bool is_active_ = false;
    LoggingStatusReason reason_ = LoggingStatusReason::Stopped;

    static_assert(
        static_cast<uint32_t>(LoggingStatusReason::Unavailable) <= UINT8_MAX,
        "A logging status reason takes one byte of the record"
    );

public:
    /** @brief Inactive, for the reason Stopped. */
    CanbusComLoggingState() = default;

    explicit CanbusComLoggingState(bool is_active, LoggingStatusReason reason = LoggingStatusReason::Stopped) :
        is_active_(is_active),
        reason_(is_active ? LoggingStatusReason::Stopped : reason) {}

    explicit CanbusComLoggingState(std::span<const uint8_t> data) {
        if(data.empty())
            return;

        is_active_ = (data[0] & FLAG_ACTIVE) != 0;
        if(!is_active_ && data.size() > 1)
            reason_ = static_cast<LoggingStatusReason>(data[1]);
    }

    [[nodiscard]] bool IsActive() const {
        return is_active_;
    }

    /** @brief Why the unit does not log; Stopped while it logs. */
    [[nodiscard]] LoggingStatusReason GetReason() const {
        return reason_;
    }

    [[nodiscard]] CanbusComStateType GetStateType() const override {
        return STATE_TYPE;
    }

    [[nodiscard]] CanbusComStateData GetData() const override {
        const std::array<uint8_t, 2> data = { static_cast<uint8_t>(is_active_ ? FLAG_ACTIVE : 0x00),
            static_cast<uint8_t>(reason_) };

        return CanbusComStateData(data);
    }

    bool operator==(const CanbusComLoggingState& other) const {
        return is_active_ == other.is_active_ && reason_ == other.reason_;
    }
};

static_assert(CanbusComState<CanbusComLoggingState>);

} // namespace eerie_leap::domain::canbus_com_domain::states
