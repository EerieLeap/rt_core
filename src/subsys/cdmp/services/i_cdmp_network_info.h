#pragma once

#include <cstddef>
#include <span>

#include "subsys/cdmp/models/cdmp_device_info.h"

namespace eerie_leap::subsys::cdmp::services {

using eerie_leap::subsys::cdmp::models::CdmpDeviceInfo;

/** @brief Read-only view of this unit and the CDMP network, safe to query from any thread. */
class ICdmpNetworkInfo {
public:
    virtual ~ICdmpNetworkInfo() = default;

    /** @brief This unit; its device ID is unassigned until one is claimed. */
    [[nodiscard]] virtual CdmpDeviceInfo GetDeviceInfo() const = 0;

    /**
     * @brief Copies the other devices, ordered by device ID, into a caller-owned buffer.
     * @return The number of devices copied, at most @p devices.size().
     */
    virtual size_t GetNetworkDevices(std::span<CdmpDeviceInfo> devices) const = 0;

    /** @brief Number of other devices on the network. */
    [[nodiscard]] virtual size_t GetNetworkDeviceCount() const = 0;
};

} // namespace eerie_leap::subsys::cdmp::services
