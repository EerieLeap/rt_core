#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "subsys/smp/smp_group.h"
#include "subsys/cdmp/services/i_cdmp_network_info.h"

namespace eerie_leap::domain::canbus_com_domain::smp {

using eerie_leap::subsys::smp::SmpGroup;
using eerie_leap::subsys::cdmp::models::CdmpDeviceInfo;
using eerie_leap::subsys::cdmp::services::ICdmpNetworkInfo;

/** @brief SMP `network` group: the other devices on the CDMP network. */
class NetworkMgmtGroup : public SmpGroup<NetworkMgmtGroup> {
public:
    /** @brief Command IDs. */
    enum class Command : uint8_t {
        /// Read `{off}` (optional): `{total, off, devices: [{id, uid, type, status}]}`, ordered by ID,
        /// as many as fit into one response; request the rest from the next offset.
        DEVICES = 0,
    };

    static constexpr size_t MAX_DEVICES = CONFIG_EERIE_LEAP_SMP_NETWORK_MAX_DEVICES;

private:
    static const mgmt_handler HANDLERS[];

    std::shared_ptr<const ICdmpNetworkInfo> network_info_;
    // Only touched on the MCUmgr work queue.
    std::array<CdmpDeviceInfo, MAX_DEVICES> devices_{};

    int HandleDevices(smp_streamer* ctxt);

public:
    explicit NetworkMgmtGroup(std::shared_ptr<const ICdmpNetworkInfo> network_info);
    ~NetworkMgmtGroup();
};

} // namespace eerie_leap::domain::canbus_com_domain::smp
