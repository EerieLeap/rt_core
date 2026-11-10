#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "domain/can_monitor_domain/models/can_monitor_types.h"
#include "domain/can_monitor_domain/services/can_monitor_service.h"
#include "subsys/smp/smp_group.h"

namespace eerie_leap::domain::can_monitor_domain::smp {

using eerie_leap::domain::can_monitor_domain::models::CanMonitorError;
using eerie_leap::domain::can_monitor_domain::models::CanUnitFilter;
using eerie_leap::domain::can_monitor_domain::services::CanMonitorService;
using eerie_leap::subsys::smp::SmpGroup;

/**
 * @brief SMP `canmon` group (74): CAN monitor sessions.
 *
 * Phase 2 implements OPEN and CLOSE; the other commands are empty entries, which MCUmgr answers with
 * ENOTSUP until their phase fills them.
 *
 * Handlers run on the MCUmgr work queue and call the service, which has its own lock.
 */
class CanMonitorMgmtGroup : public SmpGroup<CanMonitorMgmtGroup> {
public:
    /** @brief Command IDs. */
    enum class Command : uint8_t {
        /// Read (Phase 3).
        STATUS = 0,
        /// Write `{ch, mode, ? type, ? bitrate, ? data_bitrate, ? filters: [[id, mask, ext]], ? delta,
        /// ? synthetic: {fps, ids, dlc, ? ext, ? fd}}`: `{tok, ch, mode, type, bitrate, data_bitrate, lease_ms}`.
        /// With `synthetic`, `ch` and `mode` are ignored and the answer states ch 255, mode 0.
        OPEN = 1,
        /// Write `{tok}`: `{}`. BAD_TOKEN for 0, NO_SESSION for a token no session holds.
        CLOSE = 2,
        /// Write (Phase 3).
        FILTER = 3,
        /// Read (Phase 3).
        STATS = 4,
        /// Write (Phase 4).
        ARM = 5,
        SEND = 6,
        PERIODIC = 7,
        REPLAY = 8,
    };

    /** @brief The group's errors, in the SMP v2 `err` map: the service's own. */
    using Error = CanMonitorError;

    static constexpr size_t COMMAND_COUNT = 9;

    /** @brief A request's unit filters; more than fit are counted, not stored. */
    struct FilterList {
        std::array<CanUnitFilter, CONFIG_EERIE_LEAP_CAN_MONITOR_MAX_FILTERS> filters{};
        size_t count = 0;
    };

    /** @brief A request's `synthetic` map. */
    struct SyntheticRequest {
        uint32_t fps = 0;
        uint32_t ids = 0;
        uint32_t dlc = 0;
        bool ext = false;
        bool fd = false;
        /// `fps`, `ids` and `dlc` were all there.
        bool is_complete = false;
    };

private:
    static const mgmt_handler HANDLERS[COMMAND_COUNT];

    std::shared_ptr<CanMonitorService> service_;
    // Only touched on the MCUmgr work queue.
    FilterList filter_list_;

    int HandleOpen(smp_streamer* ctxt);
    int HandleClose(smp_streamer* ctxt);

    static int TranslateError(uint16_t error);

public:
    explicit CanMonitorMgmtGroup(std::shared_ptr<CanMonitorService> service);
    ~CanMonitorMgmtGroup();

    CanMonitorMgmtGroup(const CanMonitorMgmtGroup&) = delete;
    CanMonitorMgmtGroup& operator=(const CanMonitorMgmtGroup&) = delete;
};

} // namespace eerie_leap::domain::can_monitor_domain::smp
