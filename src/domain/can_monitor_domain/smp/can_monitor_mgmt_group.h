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
 * Every request that carries a token renews its session's lease. ARM, SEND, PERIODIC and REPLAY are built
 * with EERIE_LEAP_CAN_MONITOR_TX; without it they answer TX_UNSUPPORTED.
 *
 * Handlers run on the MCUmgr work queue and call the service, which has its own lock.
 */
class CanMonitorMgmtGroup : public SmpGroup<CanMonitorMgmtGroup> {
public:
    /** @brief Command IDs. */
    enum class Command : uint8_t {
        /// Read `{}`: `{version, ports: [{ch, types: [uint], configured, state, bitrate, data_bitrate, session}],
        /// filters_max, ring_bytes, notify_max, tx, synthetic, delta}`.
        STATUS = 0,
        /// Write `{ch, mode, ? type, ? bitrate, ? data_bitrate, ? filters: [[id, mask, ext]], ? delta,
        /// ? synthetic: {fps, ids, dlc, ? ext, ? fd}}`: `{tok, ch, mode, type, bitrate, data_bitrate, lease_ms}`.
        /// With `synthetic`, `ch` and `mode` are ignored and the answer states ch 255, mode 0.
        OPEN = 1,
        /// Write `{tok}`: `{}`. BAD_TOKEN for 0, NO_SESSION for a token no session holds.
        CLOSE = 2,
        /// Write `{tok, filters: [[id, mask, ext]]}`: `{}`. Replaces the unit filters; an empty list passes
        /// every frame; more than `filters_max` is EINVAL.
        FILTER = 3,
        /// Read `{tok}`: `{state, tec, rec, rx, tx, busy_us, errors: [bit, stuff, crc, form, ack],
        /// lost: [controller, queue, ring, echoes], filtered}`, totals since OPEN.
        STATS = 4,
        /// Write `{tok, on}`: `{armed, share_pct, reserved: [[from, to, ext]]}`. TAP and ACTIVE sessions only
        /// (BAD_MODE otherwise). Nothing is sent before it; disarming stops the periodic frames and the replay.
        ARM = 5,
        /// Write `{tok, frames: [[id, flags, data]]}`: `{sent, refused}`. At most 16 frames; flags: IDE 1,
        /// FDF 2, BRS 4, RTR 8. The size of `data` is a remote frame's length; its bytes are not sent.
        SEND = 6,
        /// Write `{tok, list: [{slot, id, flags, data, period_ms, ? count}]}`: `{}`. Replaces the periodic frames.
        PERIODIC = 7,
        /// Write `{tok, op, ? speed_pct, ? loop, ? seq, ? data}`: `{room}`. Ops: 0 start, 1 chunk, 2 stop, 3 no
        /// more chunks.
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

#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_TX
    /** @brief SEND's frames; more than fit are counted, not stored. */
    struct TxFrameList {
        std::array<models::CanMonitorTxFrame, 16> frames{};
        size_t count = 0;
        /// A frame's data was longer than any frame carries, or a flag byte did not fit.
        bool is_valid = true;
    };

    /** @brief PERIODIC's list; more entries than fit are counted, not stored. */
    struct PeriodicList {
        std::array<models::CanMonitorPeriodic, CONFIG_EERIE_LEAP_CAN_MONITOR_PERIODIC_SLOTS> entries{};
        size_t count = 0;
        /// An entry lacked a key or had a value that does not fit.
        bool is_valid = true;
    };
#endif

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
#ifdef CONFIG_EERIE_LEAP_CAN_MONITOR_TX
    TxFrameList tx_frame_list_;
    PeriodicList periodic_list_;
#endif

    int HandleStatus(smp_streamer* ctxt);
    int HandleOpen(smp_streamer* ctxt);
    int HandleClose(smp_streamer* ctxt);
    int HandleFilter(smp_streamer* ctxt);
    int HandleStats(smp_streamer* ctxt);
    int HandleArm(smp_streamer* ctxt);
    int HandleSend(smp_streamer* ctxt);
    int HandlePeriodic(smp_streamer* ctxt);
    int HandleReplay(smp_streamer* ctxt);

    static int TranslateError(uint16_t error);

public:
    explicit CanMonitorMgmtGroup(std::shared_ptr<CanMonitorService> service);
    ~CanMonitorMgmtGroup();

    CanMonitorMgmtGroup(const CanMonitorMgmtGroup&) = delete;
    CanMonitorMgmtGroup& operator=(const CanMonitorMgmtGroup&) = delete;
};

} // namespace eerie_leap::domain::can_monitor_domain::smp
