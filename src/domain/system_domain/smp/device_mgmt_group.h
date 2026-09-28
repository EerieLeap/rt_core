#pragma once

#include <cstdint>
#include <memory>

#include "subsys/smp/smp_group.h"
#include "subsys/cdmp/services/i_cdmp_network_info.h"

namespace eerie_leap::domain::system_domain::smp {

using eerie_leap::subsys::smp::SmpGroup;
using eerie_leap::subsys::cdmp::services::ICdmpNetworkInfo;

/** @brief SMP `device` group: what this unit is and which CDMP identity it has. */
class DeviceMgmtGroup : public SmpGroup<DeviceMgmtGroup> {
public:
    /** @brief Command IDs. */
    enum class Command : uint8_t {
        /// Read: `{family, product, revision, features, hw, sw, build}`, versions packed as
        /// `major << 24 | minor << 16 | patch`, plus `{cdmp_id, uid, device_type, status}` with CDMP.
        INFO = 0,
    };

private:
    static const mgmt_handler HANDLERS[];

    uint32_t build_number_;
    std::shared_ptr<const ICdmpNetworkInfo> network_info_;

    int HandleInfo(smp_streamer* ctxt);

public:
    /**
     * @param build_number From the system configuration, captured once.
     * @param network_info Source of the CDMP identity; nullptr on a unit without CDMP.
     */
    DeviceMgmtGroup(uint32_t build_number, std::shared_ptr<const ICdmpNetworkInfo> network_info);
    ~DeviceMgmtGroup();
};

} // namespace eerie_leap::domain::system_domain::smp
