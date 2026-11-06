#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "domain/system_domain/models/sensor_capabilities.h"
#include "subsys/cdmp/services/i_cdmp_network_info.h"
#include "subsys/smp/smp_group.h"
#include "subsys/time/i_time_service.h"

namespace eerie_leap::domain::system_domain::smp {

using eerie_leap::domain::system_domain::models::SensorCapabilities;
using eerie_leap::subsys::cdmp::services::ICdmpNetworkInfo;
using eerie_leap::subsys::smp::SmpGroup;
using eerie_leap::subsys::time::ITimeService;

/** @brief SMP `device` group: what this unit is and which CDMP identity it has. */
class DeviceMgmtGroup : public SmpGroup<DeviceMgmtGroup> {
public:
    /** @brief Command IDs. */
    enum class Command : uint8_t {
        /// Read: `{product, revision, features, hw, sw, build, board}`, versions packed as
        /// `major << 24 | minor << 16 | patch`, `build` the application VERSION file's tweak and
        /// `board` the text `CONFIG_BOARD_TARGET`, plus `{cdmp_id, uid, status}` with CDMP
        /// and `{adc_channels, gpio_channels, sensors_max}` with sensors. `features` is the
        /// ProductFeature mask this unit sends on CDMP now (LOGGING only while a log writer is
        /// available), or the build's features without CDMP. Always present:
        /// `event_channels: [uint]`, the EventChannelId of each channel this unit registered (empty
        /// without the event bus), and `settings: [tstr]`, the ids of the settings it carries.
        INFO = 0,
        /// Read: `{now_ms, uptime_ms, boot_id}` and `wall_ms`, read together. `now_ms` is the unit's
        /// clock as Unix milliseconds, `uptime_ms` is `k_uptime_get()`, `boot_id` tells this power-on
        /// from another and `wall_ms` is real time, present only once SET_TIME has set it. Answers
        /// ENOTSUP without a time service.
        TIME = 1,
        /// Write: `{wall_ms: uint64}`, real time as Unix milliseconds, taken as the time the request
        /// arrives. EINVAL when it is missing, before 2020-01-01T00:00:00Z or after 2100-01-01T00:00:00Z. Answers as TIME, after
        /// the set. ENOTSUP without a time service.
        SET_TIME = 2,
    };

private:
    static const mgmt_handler HANDLERS[];

    std::shared_ptr<const ICdmpNetworkInfo> network_info_;
    std::optional<SensorCapabilities> sensor_capabilities_;
    std::span<const std::string_view> setting_ids_;
    std::shared_ptr<ITimeService> time_service_;

    static bool PutEventChannels(zcbor_state_t* zse);
    bool PutSettings(zcbor_state_t* zse) const;
    int HandleInfo(smp_streamer* ctxt);
    int HandleTime(smp_streamer* ctxt);
    int HandleSetTime(smp_streamer* ctxt);
    int PutTime(smp_streamer* ctxt) const;

public:
    /**
     * @param network_info Source of the CDMP identity; nullptr on a unit without CDMP.
     * @param sensor_capabilities The unit's sensor inputs; nothing on a unit without sensors.
     * @param setting_ids The ids of the settings the unit carries; must outlive the group. None by default.
     * @param time_service The clock TIME and SET_TIME serve; nullptr on a unit without one, which then
     *        answers both with ENOTSUP.
     */
    explicit DeviceMgmtGroup(
        std::shared_ptr<const ICdmpNetworkInfo> network_info,
        std::optional<SensorCapabilities> sensor_capabilities = std::nullopt,
        std::span<const std::string_view> setting_ids = {},
        std::shared_ptr<ITimeService> time_service = nullptr
    );
    ~DeviceMgmtGroup();

    DeviceMgmtGroup(const DeviceMgmtGroup&) = delete;
    DeviceMgmtGroup& operator=(const DeviceMgmtGroup&) = delete;
};

} // namespace eerie_leap::domain::system_domain::smp
