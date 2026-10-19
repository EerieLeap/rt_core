#pragma once

#include <cstdint>
#include <memory>

#include "i_logging_control.h"

namespace eerie_leap::domain::logging_domain::control {

// The logging control of a unit with a log writer that can also start and stop the other units. It
// routes a request by its target and holds no state or thread of its own, so it is callable from any
// thread:
// - ALL_UNITS: this unit's writer, and every other unit that logs (one broadcast, through the network
//   half), so a trigger on a DAQ starts all of them;
// - this unit's own UID: the writer only;
// - another UID: that unit only, through the network half.
// Without a network half (no COM service) only the writer is served, and a request for another UID is
// logged and dropped.
//
// A command received from another unit goes to the writer; a stop also tells the network half to drop
// its wanted starts, so no re-assertion undoes it. IsActive() is the writer's.
class WriterLoggingControl final : public ILoggingControl {
public:
    /**
     * @param local The unit's writer (LogWriterSupervisor). Required: throws std::invalid_argument.
     * @param network The control that sends to other units (RemoteLoggingControl in Mode::Peer), already
     *        initialized; null without a COM service.
     * @param own_uid This unit's CDMP UID, 0 without a COM service.
     */
    WriterLoggingControl(
        std::shared_ptr<ILoggingControl> local,
        std::shared_ptr<ILoggingControl> network,
        uint32_t own_uid
    );

    void RequestStart(uint32_t target_uid) override;
    void RequestStop(uint32_t target_uid) override;
    // Returns the writer's answer.
    bool HandleLoggingCommand(bool is_start) override;
    [[nodiscard]] bool IsActive() const override;
    [[nodiscard]] LoggingStatusReason GetReason() const override;

private:
    std::shared_ptr<ILoggingControl> local_;
    std::shared_ptr<ILoggingControl> network_;
    uint32_t own_uid_;

    void Route(uint32_t target_uid, bool is_start);
};

} // namespace eerie_leap::domain::logging_domain::control
