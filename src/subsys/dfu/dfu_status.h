#pragma once

#include <cstdint>
#include <optional>

namespace eerie_leap::subsys::dfu {

/** @brief The confirmation state of the running image; the dfu group's STATUS `state`. Append only. */
enum class DfuState : uint32_t {
    /// The running image is confirmed; nothing is armed.
    CONFIRMED = 0,
    /// A test boot: the watchdog is armed, boot complete has not been reported yet.
    VERIFYING = 1,
    /// Boot complete was reported; the image confirms itself after the delay.
    CONFIRMING = 2,
    /// The deadline passed or a probe stuck: the watchdog is no longer fed and a reset follows.
    FAILED = 3,
};

/** @brief What the dfu group's STATUS reports. */
struct DfuStatus {
    bool confirmed = true;
    DfuState state = DfuState::CONFIRMED;
    /// Time left until self-confirmation, only in CONFIRMING.
    std::optional<uint32_t> confirm_in_ms;
    /// The hwinfo reset cause bits of this boot.
    uint32_t reset_cause = 0;
};

/** @brief Source of the dfu group's STATUS. */
class IDfuStatusSource {
public:
    IDfuStatusSource() = default;
    IDfuStatusSource(const IDfuStatusSource&) = delete;
    IDfuStatusSource& operator=(const IDfuStatusSource&) = delete;
    IDfuStatusSource(IDfuStatusSource&&) = delete;
    IDfuStatusSource& operator=(IDfuStatusSource&&) = delete;
    virtual ~IDfuStatusSource() = default;

    /** Called on the MCUmgr work queue: must not block, allocate or throw. */
    [[nodiscard]] virtual DfuStatus GetStatus() const = 0;
};

} // namespace eerie_leap::subsys::dfu
