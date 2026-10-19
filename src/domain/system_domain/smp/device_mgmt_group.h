#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "domain/system_domain/models/sensor_capabilities.h"
#include "subsys/cdmp/services/i_cdmp_network_info.h"
#include "subsys/smp/smp_group.h"

namespace eerie_leap::domain::system_domain::smp {

using eerie_leap::domain::system_domain::models::SensorCapabilities;
using eerie_leap::subsys::cdmp::services::ICdmpNetworkInfo;
using eerie_leap::subsys::smp::SmpGroup;

/** @brief SMP `device` group: what this unit is and which CDMP identity it has. */
class DeviceMgmtGroup : public SmpGroup<DeviceMgmtGroup> {
public:
    /** @brief Command IDs. */
    enum class Command : uint8_t {
        /// Read: `{product, revision, features, hw, sw, build}`, versions packed as
        /// `major << 24 | minor << 16 | patch`, plus `{cdmp_id, uid, status}` with CDMP
        /// and `{adc_channels, gpio_channels, sensors_max}` with sensors. `features` is the
        /// ProductFeature mask this unit sends on CDMP now (LOGGING only while a log writer is
        /// available), or the build's features without CDMP. Always present:
        /// `event_channels: [uint]`, the EventChannelId of each channel this unit registered (empty
        /// without the event bus), and `settings: [tstr]`, the ids of the settings it carries.
        INFO = 0,
    };

private:
    static const mgmt_handler HANDLERS[];

    uint32_t build_number_;
    std::shared_ptr<const ICdmpNetworkInfo> network_info_;
    std::optional<SensorCapabilities> sensor_capabilities_;
    std::span<const std::string_view> setting_ids_;

    static bool PutEventChannels(zcbor_state_t* zse);
    bool PutSettings(zcbor_state_t* zse) const;
    int HandleInfo(smp_streamer* ctxt);

public:
    /**
     * @param build_number From the system configuration, captured once.
     * @param network_info Source of the CDMP identity; nullptr on a unit without CDMP.
     * @param sensor_capabilities The unit's sensor inputs; nothing on a unit without sensors.
     * @param setting_ids The ids of the settings the unit carries; must outlive the group. None by default.
     */
    DeviceMgmtGroup(
        uint32_t build_number,
        std::shared_ptr<const ICdmpNetworkInfo> network_info,
        std::optional<SensorCapabilities> sensor_capabilities = std::nullopt,
        std::span<const std::string_view> setting_ids = {}
    );
    ~DeviceMgmtGroup();

    DeviceMgmtGroup(const DeviceMgmtGroup&) = delete;
    DeviceMgmtGroup& operator=(const DeviceMgmtGroup&) = delete;
};

} // namespace eerie_leap::domain::system_domain::smp
