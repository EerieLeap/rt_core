#pragma once

#include <cstdint>

#include "subsys/dfu/dfu_status.h"
#include "subsys/smp/smp_group.h"

namespace eerie_leap::subsys::dfu::smp {

using eerie_leap::subsys::smp::SmpGroup;

/**
 * @brief SMP `dfu` group (72): the confirmation state of the running image, which `img_mgmt` does
 *        not report, and the error codes of the identity check on uploads (DfuError, sent by the
 *        ImageUploadGuard).
 *
 * `img_mgmt` (group 1) and `os` reset (group 0) are Zephyr's and need no group of ours.
 */
class DfuMgmtGroup : public SmpGroup<DfuMgmtGroup> {
public:
    /** @brief Command IDs. */
    enum class Command : uint8_t {
        /// Read, empty body: `{confirmed: bool, state: uint, confirm_in_ms?: uint, reset_cause: uint}`.
        /// `confirmed` is whether the running image is confirmed (permanent). `state` is a DfuState:
        /// CONFIRMED 0, VERIFYING 1 (a test boot waiting for boot complete), CONFIRMING 2 (boot
        /// complete, counting the delay) or FAILED 3 (the deadline passed, a reset follows).
        /// `confirm_in_ms`, only in CONFIRMING, is the time left until the image confirms itself.
        /// `reset_cause` is the hwinfo reset cause mask of this boot.
        STATUS = 0,
    };

private:
    static const mgmt_handler HANDLERS[];

    const IDfuStatusSource& status_source_;

    int HandleStatus(smp_streamer* ctxt);

    static int TranslateError(uint16_t error);

public:
    /** @param status_source Must outlive the group. */
    explicit DfuMgmtGroup(const IDfuStatusSource& status_source);
    ~DfuMgmtGroup();

    DfuMgmtGroup(const DfuMgmtGroup&) = delete;
    DfuMgmtGroup& operator=(const DfuMgmtGroup&) = delete;
    DfuMgmtGroup(DfuMgmtGroup&&) = delete;
    DfuMgmtGroup& operator=(DfuMgmtGroup&&) = delete;
};

} // namespace eerie_leap::subsys::dfu::smp
